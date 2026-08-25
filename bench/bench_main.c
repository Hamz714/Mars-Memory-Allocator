// Benchmark driver.
//
// Runs each workload against each allocator, repeatedly, and writes one CSV
// row per repetition. Aggregation is deliberately left to the reporting step:
// the raw repetitions are what let a reader see the spread rather than trust a
// single number.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bench.h"
#include "mars/allocator.h"

#define DEFAULT_ARENA (64u * 1024u * 1024u)
#define DEFAULT_OPS 200000
#define DEFAULT_REPS 11

static const char *g_git_sha = "unknown";

static void write_header(FILE *out) {
  fprintf(out,
          "workload,allocator,rep,ops,ns_total,ops_per_sec,"
          "mean_ns,p50_ns,p99_ns,p999_ns,max_ns,timer_overhead_ns,"
          "peak_payload_bytes,peak_block_bytes,peak_blocks,util_pct,"
          "meta_bytes_per_alloc,quarantined,arena_bytes,seed\n");
}

// --- One repetition ---------------------------------------------------------

static void run_one(FILE *out, const bench_workload *wl, const bench_alloc *al,
                    void *arena, size_t arena_size, uint64_t ops,
                    uint64_t seed, int rep, int quiet) {
  // Static rather than automatic: the histogram is well over a hundred
  // kilobytes, which is more than belongs on a thread stack.
  static bench_hist hist;
  bench_hist_reset(&hist);

  bench_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.alloc = al;
  ctx.arena = arena;
  ctx.arena_size = arena_size;
  ctx.ops = ops;
  ctx.seed = seed;
  ctx.hist = &hist;
  mars_rng_seed(&ctx.rng, seed);

  if (al->setup(arena, arena_size) != 0) {
    fprintf(stderr, "setup failed for %s/%s\n", wl->name, al->name);
    return;
  }
  mm_stats_reset();

  uint64_t w0 = bench_wall_ns();
  wl->run(&ctx);
  uint64_t w1 = bench_wall_ns();

  uint64_t elapsed = w1 - w0;
  double ops_per_sec =
      elapsed == 0 ? 0.0 : (double)ctx.performed * 1e9 / (double)elapsed;

  uint64_t peak_payload = 0, peak_block = 0, peak_blocks = 0, quarantined = 0;
  if (al->stats != NULL) {
    al->stats(&peak_payload, &peak_block, &peak_blocks, &quarantined);
  }

  // Utilisation is payload over what those blocks actually occupied, so it
  // captures metadata and rounding together.
  double util = peak_block == 0
                    ? 0.0
                    : 100.0 * (double)peak_payload / (double)peak_block;

  // Overhead per allocation, from the same snapshot: everything the blocks
  // occupied beyond the payload, divided across the blocks holding it.
  double meta_per_alloc =
      peak_blocks == 0
          ? 0.0
          : (double)(peak_block - peak_payload) / (double)peak_blocks;

  const bench_timer_info *ti = bench_timer_init();

  fprintf(out,
          "%s,%s,%d,%llu,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.2f,"
          "%llu,%llu,%llu,%.2f,%.1f,%llu,%llu,%llu\n",
          wl->name, al->name, rep, (unsigned long long)ctx.performed,
          (unsigned long long)elapsed, ops_per_sec,
          bench_ticks_to_ns((uint64_t)bench_hist_mean(&hist)),
          bench_ticks_to_ns(bench_hist_quantile(&hist, 0.50)),
          bench_ticks_to_ns(bench_hist_quantile(&hist, 0.99)),
          bench_ticks_to_ns(bench_hist_quantile(&hist, 0.999)),
          bench_ticks_to_ns(hist.max), ti->overhead_ns,
          (unsigned long long)peak_payload, (unsigned long long)peak_block,
          (unsigned long long)peak_blocks, util, meta_per_alloc,
          (unsigned long long)quarantined, (unsigned long long)arena_size,
          (unsigned long long)seed);
  fflush(out);

  if (!quiet) {
    printf("  %-17s %-7s rep %-2d  %9.0f ops/s  p50 %6.1f ns  p99 %7.1f ns\n",
           wl->name, al->name, rep, ops_per_sec,
           bench_ticks_to_ns(bench_hist_quantile(&hist, 0.50)),
           bench_ticks_to_ns(bench_hist_quantile(&hist, 0.99)));
    fflush(stdout);
  }
}

// --- Driver -----------------------------------------------------------------

static void usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("  --out FILE       write CSV here (default: stdout)\n");
  printf("  --ops N          operations per repetition (default %d)\n",
         DEFAULT_OPS);
  printf("  --reps N         repetitions, first discarded (default %d)\n",
         DEFAULT_REPS);
  printf("  --arena N        arena bytes (default %u)\n", DEFAULT_ARENA);
  printf("  --seed N         base seed (default 20260809)\n");
  printf("  --workload NAME  run only this workload\n");
  printf("  --allocator NAME 'mars', 'system', or 'both' (default both)\n");
  printf("  --git-sha SHA    recorded in the CSV header\n");
  printf("  --list           list workloads and exit\n");
  printf("  --quiet          CSV only, no progress lines\n");
}

int main(int argc, char **argv) {
  const char *out_path = NULL;
  const char *only_workload = NULL;
  const char *only_alloc = "both";
  uint64_t ops = DEFAULT_OPS;
  uint64_t seed = 20260809;
  size_t arena_size = DEFAULT_ARENA;
  int reps = DEFAULT_REPS;
  int quiet = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--out") && i + 1 < argc) out_path = argv[++i];
    else if (!strcmp(a, "--ops") && i + 1 < argc) ops = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
    else if (!strcmp(a, "--arena") && i + 1 < argc) arena_size = (size_t)strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--seed") && i + 1 < argc) seed = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--workload") && i + 1 < argc) only_workload = argv[++i];
    else if (!strcmp(a, "--allocator") && i + 1 < argc) only_alloc = argv[++i];
    else if (!strcmp(a, "--git-sha") && i + 1 < argc) g_git_sha = argv[++i];
    else if (!strcmp(a, "--quiet")) quiet = 1;
    else if (!strcmp(a, "--list")) {
      for (size_t w = 0; w < bench_workload_count; w++) {
        printf("%-18s %s\n", bench_workloads[w].name,
               bench_workloads[w].description);
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

  void *arena = malloc(arena_size);
  if (arena == NULL) {
    fprintf(stderr, "could not allocate a %zu byte arena\n", arena_size);
    return 2;
  }

  FILE *out = stdout;
  if (out_path != NULL) {
    out = fopen(out_path, "w");
    if (out == NULL) {
      fprintf(stderr, "could not open %s for writing\n", out_path);
      free(arena);
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
    if (ti->tsc_flags_known && !ti->tsc_usable) {
      printf("warning: the timestamp counter is not invariant here, so "
             "per-operation latencies are not reliable\n");
    }
  }

  bench_write_env(out, g_git_sha);
  write_header(out);

  const bench_alloc *allocs[2];
  size_t alloc_count = 0;
  if (!strcmp(only_alloc, "both") || !strcmp(only_alloc, "mars")) {
    allocs[alloc_count++] = &bench_alloc_mars;
  }
  if (!strcmp(only_alloc, "both") || !strcmp(only_alloc, "system")) {
    allocs[alloc_count++] = &bench_alloc_system;
  }

  for (size_t w = 0; w < bench_workload_count; w++) {
    const bench_workload *wl = &bench_workloads[w];
    if (only_workload != NULL && strcmp(only_workload, wl->name) != 0) continue;

    for (size_t a = 0; a < alloc_count; a++) {
      // Repetition 0 is a warmup and is not written out: it pays for page
      // faults on the arena and for whatever the caches had to learn.
      for (int rep = 0; rep <= reps; rep++) {
        if (rep == 0) {
          static bench_hist warm;
          bench_hist_reset(&warm);
          bench_ctx c;
          memset(&c, 0, sizeof(c));
          c.alloc = allocs[a];
          c.arena = arena;
          c.arena_size = arena_size;
          c.ops = ops / 4;
          c.hist = &warm;
          mars_rng_seed(&c.rng, seed);
          if (allocs[a]->setup(arena, arena_size) == 0) wl->run(&c);
          continue;
        }
        run_one(out, wl, allocs[a], arena, arena_size, ops, seed + (uint64_t)rep,
                rep, quiet);
      }
    }
  }

  if (out != stdout) fclose(out);
  free(arena);
  return 0;
}
