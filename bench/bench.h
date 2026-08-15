// Benchmark harness: allocator abstraction and workload interface.

#ifndef BENCH_H_
#define BENCH_H_

#include <stddef.h>
#include <stdint.h>

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
// workload rather than adding a call to every measured operation.
#define BENCH_TIMED(ctx_, expr_)                       \
  do {                                                 \
    uint64_t t0_ = bench_ticks();                      \
    (expr_);                                           \
    uint64_t t1_ = bench_ticks();                      \
    bench_hist_record((ctx_)->hist, t1_ - t0_);        \
    (ctx_)->performed++;                               \
  } while (0)

#endif  // BENCH_H_
