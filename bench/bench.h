// Benchmark harness: allocator abstraction and workload interface.

#ifndef BENCH_H_
#define BENCH_H_

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "bench_timer.h"
#include "mars_rng.h"

// Every workload drives an allocator through this, so the same code measures
// this allocator and the system one and no difference can come from the
// driver.
typedef struct bench_alloc {
  const char *name;
  int (*setup)(void *arena, size_t arena_size);
  void *(*alloc)(size_t size);
  void (*release)(void *ptr);
  void *(*resize)(void *ptr, size_t size);
  // Optional; leave NULL when the allocator cannot report them. The three
  // peak figures are one consistent snapshot, taken when occupancy was
  // highest.
  void (*stats)(uint64_t *peak_payload, uint64_t *peak_block,
                uint64_t *peak_blocks, uint64_t *quarantined);
} bench_alloc;

extern const bench_alloc bench_alloc_mars;
extern const bench_alloc bench_alloc_system;

// The `#` provenance block every CSV opens with: machine, kernel, compiler,
// commit, timer calibration, profile, counters and locking strategy.
void bench_write_env(FILE *out, const char *git_sha);

// The size distribution the frozen workloads draw from: mostly small,
// occasionally large. Shared with the multithreaded workloads so that
// `mt_churn` really is the existing `churn` workload run T times over, rather
// than something that resembles it.
size_t bench_draw_size(mars_rng *r);

typedef struct bench_ctx {
  const bench_alloc *alloc;
  void *arena;
  size_t arena_size;
  uint64_t ops;      // how much work the workload should do
  uint64_t seed;
  mars_rng rng;
  bench_hist *hist;  // per-operation latency, in ticks
  uint64_t performed;
} bench_ctx;

typedef struct bench_workload {
  const char *name;
  const char *description;
  void (*run)(bench_ctx *ctx);
} bench_workload;

extern const bench_workload bench_workloads[];
extern const size_t bench_workload_count;

// Times one call and records it. Kept in the header so it inlines into the
// workload rather than adding a call to every measured operation. Used by the
// multithreaded workloads too: bench_mt_ctx carries the same two fields, and a
// second macro that did the same thing would be a second place to get it
// wrong.
#define BENCH_TIMED(ctx_, expr_)                       \
  do {                                                 \
    uint64_t t0_ = bench_ticks();                      \
    (expr_);                                           \
    uint64_t t1_ = bench_ticks();                      \
    bench_hist_record((ctx_)->hist, t1_ - t0_);        \
    (ctx_)->performed++;                               \
  } while (0)

// --- Multithreaded workloads -----------------------------------------------
//
// A separate driver and a separate table, because the question is a different
// one. The single-threaded workloads above ask what one operation costs; these
// ask what happens to that cost when T threads do it at once, which is only
// meaningful as a curve over T.

typedef struct bench_mt_ctx {
  const bench_alloc *alloc;
  unsigned threads;   // T for this run
  unsigned index;     // 0 .. T-1, and what decides a thread's role
  uint64_t ops;       // timed operations this thread should perform
  uint64_t seed;
  mars_rng rng;
  bench_hist *hist;   // this thread's own, merged after the join
  uint64_t performed;
  uint64_t failures;  // allocations that came back NULL
  void *shared;       // whatever the workload set up for all its threads
} bench_mt_ctx;

typedef struct bench_mt_workload {
  const char *name;
  const char *description;
  // Built before the threads start and taken down after they join. NULL is a
  // legitimate return only when `setup` is NULL.
  void *(*setup)(unsigned threads, uint64_t ops_per_thread);
  void (*teardown)(void *shared);
  void (*run)(bench_mt_ctx *ctx);
} bench_mt_workload;

extern const bench_mt_workload bench_mt_workloads[];
extern const size_t bench_mt_workload_count;

// The allocator bindings the multithreaded driver uses. Identical to the two
// above except in how `mars` is set up -- see workloads_mt.c, which explains
// why a caller-supplied buffer is the wrong thing to measure thread scaling
// against.
extern const bench_alloc bench_mt_alloc_mars;
extern const bench_alloc bench_mt_alloc_system;

#endif  // BENCH_H_
