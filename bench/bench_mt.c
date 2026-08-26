// The multithreaded benchmark driver.
//
// Runs each multithreaded workload against each allocator at each thread count,
// repeatedly, and writes one CSV row per repetition. Aggregation is left to
// tools/report.py for the same reason the single-threaded driver leaves it
// there: the raw repetitions are what let a reader see the spread rather than
// trust one number.
//
// A separate binary from `bench` rather than a mode of it. The two answer
// different questions -- one asks what an operation costs, the other asks what
// happens to that cost under T threads -- and they have different CSV shapes,
// different parameters and different failure modes. Folding them together would
// mean a `threads` column that is 1 in every row of nine tenths of the files.

#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "mars/allocator.h"
#include "mm_lock.h"

#define DEFAULT_OPS 200000
#define DEFAULT_REPS 7
#define MAX_THREADS 64

static const char *g_git_sha = "unknown";

// Every thread spins here until the last one has been created, so that the wall
// clock covers T threads running together rather than the first one running
// alone while the eighth is still being created. At 200,000 operations a thread
// that is a millisecond late is noise -- but a curve is exactly the thing a
// systematic millisecond would bend.
static _Atomic int g_go;

typedef struct runner {
  pthread_t id;
  bench_mt_ctx ctx;
  const bench_mt_workload *wl;
} runner;

static void *thread_main(void *arg) {
  runner *r = (runner *)arg;
  while (atomic_load_explicit(&g_go, memory_order_acquire) == 0) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }
  r->wl->run(&r->ctx);
  return NULL;
}

// --- One repetition ---------------------------------------------------------

static void run_one(FILE *out, const bench_mt_workload *wl,
                    const bench_alloc *al, unsigned threads, uint64_t ops,
                    uint64_t seed, int rep, int quiet, int write_row) {
  if (al->setup(NULL, 0) != 0) {
    fprintf(stderr, "setup failed for %s/%s\n", wl->name, al->name);
    return;
  }
  mm_stats_reset();

  runner *r = (runner *)calloc(threads, sizeof(runner));
  // One histogram per thread. Sharing one would put a write to the same cache
  // line inside every timed operation, and the harness would be measuring the
  // contention it had introduced itself.
  bench_hist *hists = (bench_hist *)calloc(threads, sizeof(bench_hist));
  if (r == NULL || hists == NULL) {
    fprintf(stderr, "out of memory for %u threads\n", threads);
    free(r);
    free(hists);
    return;
  }

  void *shared = wl->setup != NULL ? wl->setup(threads, ops) : NULL;
  if (wl->setup != NULL && shared == NULL) {
    fprintf(stderr, "workload setup failed for %s\n", wl->name);
    free(r);
    free(hists);
    return;
  }

  atomic_store_explicit(&g_go, 0, memory_order_release);
  for (unsigned i = 0; i < threads; i++) {
    bench_hist_reset(&hists[i]);
    r[i].wl = wl;
    r[i].ctx.alloc = al;
    r[i].ctx.threads = threads;
    r[i].ctx.index = i;
    r[i].ctx.ops = ops;
    r[i].ctx.seed = seed + i;
    r[i].ctx.hist = &hists[i];
    r[i].ctx.shared = shared;
    // Per thread, so that a thread's request sequence replays from its own
    // seed and does not depend on how the scheduler interleaved the run.
    mars_rng_seed(&r[i].ctx.rng, seed + i);
    if (pthread_create(&r[i].id, NULL, thread_main, &r[i]) != 0) {
      fprintf(stderr, "could not create thread %u\n", i);
      atomic_store_explicit(&g_go, 1, memory_order_release);
      for (unsigned j = 0; j < i; j++) pthread_join(r[j].id, NULL);
      if (wl->teardown != NULL) wl->teardown(shared);
      free(r);
      free(hists);
      return;
    }
  }

  uint64_t w0 = bench_wall_ns();
  atomic_store_explicit(&g_go, 1, memory_order_release);
  for (unsigned i = 0; i < threads; i++) pthread_join(r[i].id, NULL);
  uint64_t w1 = bench_wall_ns();

  if (wl->teardown != NULL) wl->teardown(shared);

  static bench_hist all;
  bench_hist_reset(&all);
  uint64_t performed = 0;
  uint64_t failures = 0;
  for (unsigned i = 0; i < threads; i++) {
    bench_hist_merge(&all, &hists[i]);
    performed += r[i].ctx.performed;
    failures += r[i].ctx.failures;
  }
  free(r);
  free(hists);

  if (!write_row) return;

  uint64_t elapsed = w1 - w0;
  double ops_per_sec =
      elapsed == 0 ? 0.0 : (double)performed * 1e9 / (double)elapsed;

  uint64_t peak_payload = 0, peak_block = 0, peak_blocks = 0, quarantined = 0;
  if (al->stats != NULL) {
    al->stats(&peak_payload, &peak_block, &peak_blocks, &quarantined);
  }

  const bench_timer_info *ti = bench_timer_init();
  fprintf(out,
          "%s,%s,%s,%u,%d,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,"
          "%.2f,%llu,%llu,%llu,%llu,%llu\n",
          wl->name, al->name, mm_lock_strategy(), threads, rep,
          (unsigned long long)performed, (unsigned long long)elapsed,
          ops_per_sec, ops_per_sec / (double)threads,
          bench_ticks_to_ns((uint64_t)bench_hist_mean(&all)),
          bench_ticks_to_ns(bench_hist_quantile(&all, 0.50)),
          bench_ticks_to_ns(bench_hist_quantile(&all, 0.99)),
          bench_ticks_to_ns(bench_hist_quantile(&all, 0.999)),
          bench_ticks_to_ns(all.max), ti->overhead_ns,
          (unsigned long long)failures, (unsigned long long)peak_payload,
          (unsigned long long)peak_block, (unsigned long long)peak_blocks,
          (unsigned long long)seed);
  fflush(out);

  if (!quiet) {
    printf("  %-18s %-7s T=%-2u rep %-2d  %10.0f ops/s  p50 %6.1f ns  "
           "p99 %8.1f ns\n",
           wl->name, al->name, threads, rep, ops_per_sec,
           bench_ticks_to_ns(bench_hist_quantile(&all, 0.50)),
           bench_ticks_to_ns(bench_hist_quantile(&all, 0.99)));
    fflush(stdout);
  }
}

// --- Driver -----------------------------------------------------------------

static void usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("  --out FILE        write CSV here (default: stdout)\n");
  printf("  --ops N           timed operations per thread (default %d)\n",
         DEFAULT_OPS);
  printf("  --reps N          repetitions, first discarded (default %d)\n",
         DEFAULT_REPS);
  printf("  --threads LIST    comma-separated thread counts "
         "(default 1,2,4,8)\n");
  printf("  --seed N          base seed (default 20260809)\n");
  printf("  --workload NAME   run only this workload\n");
  printf("  --allocator NAME  'mars', 'system', or 'both' (default both)\n");
  printf("  --git-sha SHA     recorded in the CSV header\n");
  printf("  --list            list workloads and exit\n");
  printf("  --quiet           CSV only, no progress lines\n");
}

// "1,2,4,8" into the array. Returns how many were read, or 0 on anything
// malformed -- a thread count nobody can parse is better refused than guessed.
static unsigned parse_threads(const char *spec, unsigned *out) {
  unsigned n = 0;
  const char *p = spec;
  while (*p != '\0' && n < MAX_THREADS) {
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p || v == 0 || v > MAX_THREADS) return 0;
    out[n++] = (unsigned)v;
    p = end;
    if (*p == ',') p++;
    else if (*p != '\0') return 0;
  }
  return n;
}

int main(int argc, char **argv) {
  const char *out_path = NULL;
  const char *only_workload = NULL;
  const char *only_alloc = "both";
  const char *thread_spec = "1,2,4,8";
  uint64_t ops = DEFAULT_OPS;
  uint64_t seed = 20260809;
  int reps = DEFAULT_REPS;
  int quiet = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--out") && i + 1 < argc) out_path = argv[++i];
    else if (!strcmp(a, "--ops") && i + 1 < argc) ops = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    else if (!strcmp(a, "--threads") && i + 1 < argc) thread_spec = argv[++i];
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--workload") && i + 1 < argc) only_workload = argv[++i];
    else if (!strcmp(a, "--allocator") && i + 1 < argc) only_alloc = argv[++i];
    else if (!strcmp(a, "--git-sha") && i + 1 < argc) g_git_sha = argv[++i];
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "--list")) {
      for (size_t w = 0; w < bench_mt_workload_count; w++) {
        printf("%-20s %s\n", bench_mt_workloads[w].name,
               bench_mt_workloads[w].description);
      }
      return 0;
    } else if (!strcmp(a, "--help")) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown argument: %s\n", a);
      usage(argv[0]);
      return 2;
    }
  }

  unsigned threads[MAX_THREADS];
  unsigned thread_count = parse_threads(thread_spec, threads);
  if (thread_count == 0) {
    fprintf(stderr, "could not read --threads %s\n", thread_spec);
    return 2;
  }

  // Locking compiled out means one arena and no mutex, so a second thread here
  // is two threads mutating the same bins and the same tiling. That does not
  // make a slow benchmark, it makes an invalid one: the row still prints, with
  // ops/sec computed off a heap both threads were corrupting, and it is shaped
  // exactly like a row that means something. test_threads compiles its threaded
  // cases out under this build for the same reason. This is the one place that
  // cannot, because the count arrives at runtime.
  if (strcmp(mm_lock_strategy(), "none") == 0) {
    for (unsigned t = 0; t < thread_count; t++) {
      if (threads[t] > 1) {
        fprintf(stderr,
                "bench_mt: this build has no locking (MARS_LOCK=none) and "
                "cannot be measured above one thread\n");
        return 2;
      }
    }
  }

  FILE *out = stdout;
  if (out_path != NULL) {
    out = fopen(out_path, "w");
    if (out == NULL) {
      fprintf(stderr, "could not open %s for writing\n", out_path);
      return 2;
    }
  }

  const bench_timer_info *ti = bench_timer_init();
  if (!quiet) {
    printf("timer: %.4f ns/tick, overhead %.2f ns, tsc %s\n", ti->ns_per_tick,
           ti->overhead_ns,
           !ti->tsc_flags_known ? "unverified"
                                : (ti->tsc_usable ? "constant+nonstop"
                                                  : "NOT invariant"));
    printf("locking: %s\n", mm_lock_strategy());
  }

  bench_write_env(out, g_git_sha);
  fprintf(out, "# ops_per_thread=%llu arena=growable\n",
          (unsigned long long)ops);
  fprintf(out,
          "workload,allocator,lock,threads,rep,ops,ns_total,ops_per_sec,"
          "ops_per_sec_per_thread,mean_ns,p50_ns,p99_ns,p999_ns,max_ns,"
          "timer_overhead_ns,alloc_failures,peak_payload_bytes,"
          "peak_block_bytes,peak_blocks,seed\n");

  const bench_alloc *allocs[2];
  size_t alloc_count = 0;
  if (!strcmp(only_alloc, "both") || !strcmp(only_alloc, "mars")) {
    allocs[alloc_count++] = &bench_mt_alloc_mars;
  }
  if (!strcmp(only_alloc, "both") || !strcmp(only_alloc, "system")) {
    allocs[alloc_count++] = &bench_mt_alloc_system;
  }

  for (size_t w = 0; w < bench_mt_workload_count; w++) {
    const bench_mt_workload *wl = &bench_mt_workloads[w];
    if (only_workload != NULL && strcmp(only_workload, wl->name) != 0) continue;

    for (size_t a = 0; a < alloc_count; a++) {
      for (unsigned t = 0; t < thread_count; t++) {
        // Repetition 0 is a warmup and is not written out: it pays for the
        // first chunks being mapped and for whatever the caches had to learn.
        for (int rep = 0; rep <= reps; rep++) {
          run_one(out, wl, allocs[a], threads[t], rep == 0 ? ops / 4 : ops,
                  seed + (uint64_t)rep * 1000u, rep, quiet, rep != 0);
        }
      }
    }
  }

  if (out != stdout) fclose(out);
  return 0;
}
