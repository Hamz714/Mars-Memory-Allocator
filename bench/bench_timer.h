// Timing and latency recording for the benchmark harness.
//
// Two clocks, used for different jobs:
//
//   bench_wall_ns()  a monotonic wall clock, used to time whole batches. This
//                    is the trustworthy number and is what throughput is
//                    derived from.
//
//   bench_ticks()    the CPU timestamp counter, used for per-operation
//                    latency. The fast path is tens of nanoseconds, and a
//                    clock_gettime call costs about as much again, so timing
//                    individual operations with the wall clock would measure
//                    the clock rather than the allocator.
//
// The TSC is only trustworthy when the CPU advertises constant_tsc and
// nonstop_tsc; bench_timer_init reports what it found so a run can say so
// rather than quietly assume it.

#ifndef BENCH_TIMER_H_
#define BENCH_TIMER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Clocks ----------------------------------------------------------------

typedef struct bench_timer_info {
  bool tsc_usable;         // constant and non-stopping, as far as we can tell
  bool tsc_flags_known;    // whether those flags could be read at all
  double ns_per_tick;      // calibrated against the wall clock
  uint64_t overhead_ticks; // cost of a back-to-back pair of tick reads
  double overhead_ns;
} bench_timer_info;

// Calibrates the timestamp counter and measures its own overhead. Call once
// before timing anything.
const bench_timer_info *bench_timer_init(void);

uint64_t bench_wall_ns(void);
uint64_t bench_ticks(void);

// Converts a tick delta to nanoseconds, subtracting the measured cost of the
// surrounding pair of reads. Never returns a negative duration.
double bench_ticks_to_ns(uint64_t ticks);

// --- Latency histogram -----------------------------------------------------
//
// Log-linear buckets. Within each power of two the value is indexed by its
// next SUB_BITS bits; since the leading bit is always set, half the sub-bucket
// range is used, giving 128 buckets per octave and a worst-case relative error
// of 1/128, under 0.8%. Fixed size and pre-allocated, so recording a sample
// never allocates and never grows.

#define BENCH_HIST_SUB_BITS 8
#define BENCH_HIST_SUB_COUNT (1u << BENCH_HIST_SUB_BITS)
#define BENCH_HIST_BUCKETS 64
#define BENCH_HIST_SLOTS (BENCH_HIST_BUCKETS * BENCH_HIST_SUB_COUNT)

typedef struct bench_hist {
  uint64_t slots[BENCH_HIST_SLOTS];
  uint64_t count;
  uint64_t total;  // sum of recorded values, for the mean
  uint64_t min;
  uint64_t max;
} bench_hist;

void bench_hist_reset(bench_hist *h);
void bench_hist_record(bench_hist *h, uint64_t value);

// Smallest value at or below which `q` of the distribution falls, q in [0,1].
uint64_t bench_hist_quantile(const bench_hist *h, double q);
double bench_hist_mean(const bench_hist *h);

#endif  // BENCH_TIMER_H_
