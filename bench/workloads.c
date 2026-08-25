// Allocation workloads.
//
// These are frozen once written: changing a workload silently changes every
// number it has ever produced. Each one is seeded, so two runs of the same
// workload at the same seed perform exactly the same sequence of requests.

#include "bench.h"

#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"

#define MAX_LIVE 8192

// --- Allocator bindings ----------------------------------------------------

static int mars_setup(void *arena, size_t size) {
  return mm_init(arena, size);
}

static void mars_stats(uint64_t *peak_payload, uint64_t *peak_block,
                       uint64_t *peak_blocks, uint64_t *quarantined) {
  mm_stats_t s;
  mm_stats_get(&s);
  *peak_payload = s.peak_payload_bytes;
  *peak_block = s.peak_block_bytes;
  *peak_blocks = s.peak_blocks;
  *quarantined = s.quarantined_blocks;
}

const bench_alloc bench_alloc_mars = {
    "mars", mars_setup, mm_malloc, mm_free, mm_realloc, mars_stats};

// The system allocator manages its own memory, so the arena is ignored. It is
// measured through the identical driver purely as a reference point.
static int system_setup(void *arena, size_t size) {
  (void)arena;
  (void)size;
  return 0;
}

const bench_alloc bench_alloc_system = {
    "system", system_setup, malloc, free, realloc, NULL};

// --- Size distributions ----------------------------------------------------

// Mostly small, occasionally large: the shape most programs actually produce.
//
// Not static: bench/workloads_mt.c draws from the same distribution, so that
// `mt_churn` is the `churn` workload below run on T threads rather than
// something that merely resembles it.
size_t bench_draw_size(mars_rng *r) {
  uint64_t roll = mars_rng_below(r, 1000);
  if (roll < 900) return 1 + mars_rng_below(r, 128);
  if (roll < 999) {
    // Log-uniform across 8..4096 so each octave is equally likely.
    unsigned octave = (unsigned)mars_rng_below(r, 10);  // 2^3 .. 2^12
    size_t low = (size_t)1 << (octave + 3);
    return low + mars_rng_below(r, low);
  }
  return 65536 + mars_rng_below(r, 1024u * 1024u - 65536u);
}

// --- W1 / W2: sequential allocate, then release in a fixed order ------------

static void seq_common(bench_ctx *ctx, int lifo) {
  void **held = (void **)calloc(MAX_LIVE, sizeof(void *));
  if (held == NULL) return;

  uint64_t remaining = ctx->ops;
  while (remaining > 0) {
    size_t n = 0;
    while (n < MAX_LIVE && remaining > 0) {
      void *p = NULL;
      BENCH_TIMED(ctx, p = ctx->alloc->alloc(64));
      remaining--;
      if (p == NULL) break;
      held[n++] = p;
    }
    for (size_t i = 0; i < n; i++) {
      void *p = lifo ? held[n - 1 - i] : held[i];
      BENCH_TIMED(ctx, ctx->alloc->release(p));
    }
  }
  free(held);
}

static void w_seq_lifo(bench_ctx *ctx) { seq_common(ctx, 1); }
static void w_seq_fifo(bench_ctx *ctx) { seq_common(ctx, 0); }

// --- W3: mixed sizes, released in shuffled order ---------------------------

static void w_random_size(bench_ctx *ctx) {
  void **held = (void **)calloc(MAX_LIVE, sizeof(void *));
  if (held == NULL) return;

  uint64_t remaining = ctx->ops;
  while (remaining > 0) {
    size_t n = 0;
    while (n < MAX_LIVE && remaining > 0) {
      size_t sz = bench_draw_size(&ctx->rng);
      void *p = NULL;
      BENCH_TIMED(ctx, p = ctx->alloc->alloc(sz));
      remaining--;
      if (p == NULL) break;
      held[n++] = p;
    }
    // Fisher-Yates, so the release order bears no relation to the alloc order.
    for (size_t i = n; i > 1; i--) {
      size_t j = (size_t)mars_rng_below(&ctx->rng, i);
      void *tmp = held[i - 1];
      held[i - 1] = held[j];
      held[j] = tmp;
    }
    for (size_t i = 0; i < n; i++) {
      BENCH_TIMED(ctx, ctx->alloc->release(held[i]));
    }
  }
  free(held);
}

// --- W4: steady-state churn ------------------------------------------------

static void w_churn(bench_ctx *ctx) {
  const size_t target = 512;
  void **held = (void **)calloc(target, sizeof(void *));
  if (held == NULL) return;

  size_t live = 0;
  while (live < target) {
    void *p = ctx->alloc->alloc(bench_draw_size(&ctx->rng));
    if (p == NULL) break;
    held[live++] = p;
  }

  // From here the population size holds steady: every allocation is preceded
  // by a release, which is what keeps the allocator in its recycling regime.
  for (uint64_t i = 0; i < ctx->ops && live > 0; i++) {
    size_t slot = (size_t)mars_rng_below(&ctx->rng, live);
    BENCH_TIMED(ctx, ctx->alloc->release(held[slot]));

    void *p = NULL;
    size_t sz = bench_draw_size(&ctx->rng);
    BENCH_TIMED(ctx, p = ctx->alloc->alloc(sz));
    held[slot] = p;
    if (p == NULL) {
      held[slot] = held[--live];
    }
  }

  for (size_t i = 0; i < live; i++) ctx->alloc->release(held[i]);
  free(held);
}

// --- W5: repeated growth ---------------------------------------------------

static void w_realloc_grow(bench_ctx *ctx) {
  uint64_t done = 0;
  while (done < ctx->ops) {
    size_t n = 16;
    void *p = NULL;
    BENCH_TIMED(ctx, p = ctx->alloc->alloc(n));
    done++;
    if (p == NULL) break;

    // Vector-style growth by half each time, until it stops fitting.
    while (done < ctx->ops) {
      size_t next = n + n / 2 + 1;
      if (next > 1024u * 1024u) break;
      void *q = NULL;
      BENCH_TIMED(ctx, q = ctx->alloc->resize(p, next));
      done++;
      if (q == NULL) break;
      p = q;
      n = next;
    }
    BENCH_TIMED(ctx, ctx->alloc->release(p));
    done++;
  }
}

// --- W6: fragmentation -----------------------------------------------------

// Alternating small and large blocks, then every large one released. The
// small survivors leave the free space in pieces, so what matters here is how
// much of the arena remains usable rather than how fast it ran.
static void w_fragmentation(bench_ctx *ctx) {
  void **small = (void **)calloc(MAX_LIVE, sizeof(void *));
  void **large = (void **)calloc(MAX_LIVE, sizeof(void *));
  if (small == NULL || large == NULL) {
    free(small);
    free(large);
    return;
  }

  size_t n = 0;
  uint64_t remaining = ctx->ops;
  while (n < MAX_LIVE && remaining >= 2) {
    void *s = NULL;
    void *l = NULL;
    BENCH_TIMED(ctx, s = ctx->alloc->alloc(32));
    BENCH_TIMED(ctx, l = ctx->alloc->alloc(1024));
    remaining -= 2;
    if (s == NULL || l == NULL) {
      if (s != NULL) small[n++] = s;
      break;
    }
    small[n] = s;
    large[n] = l;
    n++;
  }

  for (size_t i = 0; i < n; i++) {
    BENCH_TIMED(ctx, ctx->alloc->release(large[i]));
  }

  // With the large blocks gone, ask for medium ones: how many the arena still
  // yields is the fragmentation signal.
  size_t recovered = 0;
  while (recovered < MAX_LIVE) {
    void *p = NULL;
    BENCH_TIMED(ctx, p = ctx->alloc->alloc(512));
    if (p == NULL) break;
    large[recovered++] = p;
  }

  for (size_t i = 0; i < recovered; i++) ctx->alloc->release(large[i]);
  for (size_t i = 0; i < n; i++) ctx->alloc->release(small[i]);
  free(small);
  free(large);
}

// --- W9: validated access --------------------------------------------------

// The one workload that measures what this allocator does differently: reads
// and writes that verify block integrity on the way through. Only meaningful
// for mars; for the system allocator it degenerates to memcpy, which is
// exactly the comparison worth having.
static void w_validated_access(bench_ctx *ctx) {
  static const size_t sizes[] = {8, 64, 1024, 65536};
  const size_t count = sizeof(sizes) / sizeof(sizes[0]);

  uint8_t *scratch = (uint8_t *)malloc(65536);
  if (scratch == NULL) return;
  memset(scratch, 0xA5, 65536);

  int is_mars = strcmp(ctx->alloc->name, "mars") == 0;

  for (uint64_t i = 0; i < ctx->ops; i++) {
    size_t n = sizes[i % count];
    void *p = ctx->alloc->alloc(n);
    if (p == NULL) continue;

    if (is_mars) {
      BENCH_TIMED(ctx, (void)mm_write(p, 0, scratch, n));
      BENCH_TIMED(ctx, (void)mm_read(p, 0, scratch, n));
    } else {
      BENCH_TIMED(ctx, memcpy(p, scratch, n));
      BENCH_TIMED(ctx, memcpy(scratch, p, n));
    }
    ctx->alloc->release(p);
  }
  free(scratch);
}

// --- Registry --------------------------------------------------------------

const bench_workload bench_workloads[] = {
    {"seq_lifo", "allocate a run of equal blocks, release newest first",
     w_seq_lifo},
    {"seq_fifo", "allocate a run of equal blocks, release oldest first",
     w_seq_fifo},
    {"random_size", "mixed sizes, released in shuffled order", w_random_size},
    {"churn", "steady population, one release per allocation", w_churn},
    {"realloc_grow", "repeated growth of a single block", w_realloc_grow},
    {"fragmentation", "interleaved sizes, then reclaim the large ones",
     w_fragmentation},
    {"validated_access", "integrity-checked reads and writes",
     w_validated_access},
};

const size_t bench_workload_count =
    sizeof(bench_workloads) / sizeof(bench_workloads[0]);
