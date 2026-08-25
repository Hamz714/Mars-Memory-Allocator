// The two multithreaded workloads.
//
// Separate from workloads.c, which opens by saying its workloads are frozen.
// These are frozen on the same terms and for the same reason, but they carry a
// queue and a thread-role convention with them, and appending that to the file
// whose first line is a promise not to change anything would blur which part of
// it the promise covers.
//
// --- What each one is for --------------------------------------------------
//
// `mt_churn` is the existing `churn` workload run on T threads at once, with
// nothing shared between them: same size distribution, same steady population,
// same one-release-per-allocation shape. Every thread allocates and frees its
// own blocks, so nothing here needs an allocator to handle a pointer crossing
// a thread boundary. What it measures is lock contention and nothing else --
// if T threads doing entirely independent work do not go T times faster, the
// allocator is the reason.
//
// `producer_consumer` is the case that breaks a naive per-thread allocator.
// Half the threads allocate, half free, and every single block is freed by a
// thread that did not allocate it. An allocator that gives each thread its own
// arena has to do something deliberate here, and what it does is the difference
// between this workload scaling and this workload serialising.
//
// --- Why the queue is a ring per pair and not one queue ---------------------
//
// A single shared queue would need a lock, and that lock would be contended by
// exactly the threads whose contention is being measured. The harness would
// then be measuring itself. One single-producer/single-consumer ring per pair
// removes the harness from the contention entirely: each ring is touched by two
// threads, its head by one of them and its tail by the other, so the only thing
// left for T threads to contend over is the allocator.

#define _POSIX_C_SOURCE 200809L

#include "bench.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_arena.h"
#include "mm_internal.h"

// --- Allocator bindings ----------------------------------------------------

// A growable arena rather than a caller-supplied buffer, and that is a
// measurement decision rather than a convenience.
//
// A buffer handed in by the caller is one fixed region. It cannot be divided
// between threads, so an allocator with per-thread arenas has nothing to give
// each thread and every thread ends up in the same one -- which is the
// behaviour of a global lock wearing per-thread clothes. Measuring thread
// scaling against a fixed arena would therefore measure the fallback rather
// than the design. A growable arena is also what a threaded program actually
// gets: it is what the preload shim installs, and the shim is the only way a
// program that was not written against this allocator ever reaches it.
//
// Under a global lock the two are identical, which is what makes the curve
// taken this way comparable across strategies.
static int mars_mt_setup(void *arena, size_t size) {
  (void)arena;
  (void)size;
  return mm_arena_init_growable();
}

static void mars_mt_stats(uint64_t *peak_payload, uint64_t *peak_block,
                          uint64_t *peak_blocks, uint64_t *quarantined) {
  mm_stats_t s;
  mm_stats_get(&s);
  *peak_payload = s.peak_payload_bytes;
  *peak_block = s.peak_block_bytes;
  *peak_blocks = s.peak_blocks;
  *quarantined = s.quarantined_blocks;
}

const bench_alloc bench_mt_alloc_mars = {
    "mars", mars_mt_setup, mm_malloc, mm_free, mm_realloc, mars_mt_stats};

static int system_mt_setup(void *arena, size_t size) {
  (void)arena;
  (void)size;
  return 0;
}

const bench_alloc bench_mt_alloc_system = {
    "system", system_mt_setup, malloc, free, realloc, NULL};

// --- MT1: T independent copies of `churn` ----------------------------------

// The same steady population per thread that the single-threaded workload
// holds, so that one thread of `mt_churn` and the `churn` row in bench-*.csv
// are doing the same thing.
#define CHURN_LIVE 512

static void w_mt_churn(bench_mt_ctx *ctx) {
  void **held = (void **)calloc(CHURN_LIVE, sizeof(void *));
  if (held == NULL) return;

  size_t live = 0;
  while (live < CHURN_LIVE) {
    void *p = ctx->alloc->alloc(bench_draw_size(&ctx->rng));
    if (p == NULL) break;
    held[live++] = p;
  }

  // Two timed operations per iteration -- one release, one allocation -- so
  // half as many iterations as the operation budget. `ops` is a count of timed
  // operations per thread under every workload here, which is what makes the
  // totals comparable between them.
  for (uint64_t i = 0; i < ctx->ops / 2 && live > 0; i++) {
    size_t slot = (size_t)mars_rng_below(&ctx->rng, live);
    BENCH_TIMED(ctx, ctx->alloc->release(held[slot]));

    void *p = NULL;
    size_t sz = bench_draw_size(&ctx->rng);
    BENCH_TIMED(ctx, p = ctx->alloc->alloc(sz));
    held[slot] = p;
    if (p == NULL) {
      ctx->failures++;
      held[slot] = held[--live];
    }
  }

  for (size_t i = 0; i < live; i++) ctx->alloc->release(held[i]);
  free(held);
}

// --- MT2: producer / consumer ----------------------------------------------

// Bounded, because an unbounded queue is a memory-growth workload wearing a
// producer/consumer costume: the producer would race ahead and the measurement
// would become one of how fast the allocator can map new memory. 8,192 in
// flight per pair is deep enough that the two threads rarely meet at either end
// of the ring and shallow enough that the live set stays a few tens of
// megabytes.
#define PC_RING 8192
#define PC_MASK (PC_RING - 1)

typedef struct pc_ring {
  void *slot[PC_RING];
  // On separate lines. The producer writes `head` and reads `tail`; the
  // consumer does the reverse, and sharing a line would make every push
  // invalidate the consumer's copy of a value it is spinning on.
  _Alignas(64) _Atomic uint64_t head;
  _Alignas(64) _Atomic uint64_t tail;
  _Alignas(64) _Atomic int closed;  // the producer has finished
} pc_ring;

typedef struct pc_shared {
  unsigned pairs;
  pc_ring *ring;
} pc_shared;

static void *pc_setup(unsigned threads, uint64_t ops_per_thread) {
  (void)ops_per_thread;
  pc_shared *sh = (pc_shared *)calloc(1, sizeof(pc_shared));
  if (sh == NULL) return NULL;
  sh->pairs = threads < 2 ? 1 : threads / 2;
  sh->ring = (pc_ring *)calloc(sh->pairs, sizeof(pc_ring));
  if (sh->ring == NULL) {
    free(sh);
    return NULL;
  }
  return sh;
}

static void pc_teardown(void *shared) {
  pc_shared *sh = (pc_shared *)shared;
  if (sh == NULL) return;
  free(sh->ring);
  free(sh);
}

// Somewhere for a spinning thread to be. There is nothing useful to do while
// waiting for the other end of the ring, and yielding to the scheduler would
// put a syscall inside the measurement.
static void pause_briefly(void) {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

static void produce(bench_mt_ctx *ctx, pc_ring *r, uint64_t count) {
  for (uint64_t i = 0; i < count; i++) {
    size_t n = bench_draw_size(&ctx->rng);
    void *p = NULL;
    BENCH_TIMED(ctx, p = ctx->alloc->alloc(n));
    if (p == NULL) {
      ctx->failures++;
      continue;
    }
    uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    while (head - atomic_load_explicit(&r->tail, memory_order_acquire) >=
           PC_RING) {
      pause_briefly();
    }
    r->slot[head & PC_MASK] = p;
    // Release, so that the consumer which sees this index also sees the slot.
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
  }
  atomic_store_explicit(&r->closed, 1, memory_order_release);
}

static void consume(bench_mt_ctx *ctx, pc_ring *r) {
  for (;;) {
    uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) {
      if (atomic_load_explicit(&r->closed, memory_order_acquire) != 0) {
        // One more look, because `closed` may have been set between the two
        // loads above and there is no second chance after this.
        if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) break;
      }
      pause_briefly();
      continue;
    }
    void *p = r->slot[tail & PC_MASK];
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
    BENCH_TIMED(ctx, ctx->alloc->release(p));
  }
}

static void w_producer_consumer(bench_mt_ctx *ctx) {
  pc_shared *sh = (pc_shared *)ctx->shared;
  if (sh == NULL) return;

  if (ctx->threads < 2) {
    // The baseline. One thread fills the ring and drains it itself, so the
    // allocation and release order is the same as the paired case and the only
    // thing missing is the thread boundary -- which is exactly the variable
    // this curve is about.
    pc_ring *r = &sh->ring[0];
    uint64_t half = ctx->ops / 2;
    for (uint64_t i = 0; i < half; i++) {
      size_t n = bench_draw_size(&ctx->rng);
      void *p = NULL;
      BENCH_TIMED(ctx, p = ctx->alloc->alloc(n));
      if (p == NULL) {
        ctx->failures++;
        continue;
      }
      uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
      uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
      if (head - tail >= PC_RING) {
        void *old = r->slot[tail & PC_MASK];
        atomic_store_explicit(&r->tail, tail + 1, memory_order_relaxed);
        BENCH_TIMED(ctx, ctx->alloc->release(old));
      }
      r->slot[head & PC_MASK] = p;
      atomic_store_explicit(&r->head, head + 1, memory_order_relaxed);
    }
    for (;;) {
      uint64_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
      uint64_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
      if (tail == head) break;
      void *old = r->slot[tail & PC_MASK];
      atomic_store_explicit(&r->tail, tail + 1, memory_order_relaxed);
      BENCH_TIMED(ctx, ctx->alloc->release(old));
    }
    return;
  }

  // Threads 0 .. pairs-1 produce; the rest consume, one per producer. A
  // consumer performs exactly as many timed operations as its producer, so
  // every thread carries the same share of the work whatever T is.
  if (ctx->index < sh->pairs) {
    produce(ctx, &sh->ring[ctx->index], ctx->ops);
  } else {
    consume(ctx, &sh->ring[ctx->index - sh->pairs]);
  }
}

// --- Registry --------------------------------------------------------------

const bench_mt_workload bench_mt_workloads[] = {
    {"mt_churn",
     "T independent copies of the churn workload; contention and nothing else",
     NULL, NULL, w_mt_churn},
    {"producer_consumer",
     "half the threads allocate and enqueue, half dequeue and free",
     pc_setup, pc_teardown, w_producer_consumer},
};

const size_t bench_mt_workload_count =
    sizeof(bench_mt_workloads) / sizeof(bench_mt_workloads[0]);
