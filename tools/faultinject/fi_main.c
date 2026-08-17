// Fault injection harness.
//
// Flips bits inside chosen allocator structures and classifies what the
// allocator then does about it. The question it answers is not "does it
// survive" but "when it does not notice, does anything actually go wrong" --
// so every trial is checked against a shadow model of what the payloads are
// supposed to contain. A flip the allocator missed that changed nothing is a
// different outcome from one it missed that corrupted data, and lumping them
// together would make the numbers meaningless.
//
// Each trial runs in a forked child under an alarm, because a good number of
// them are expected to crash or hang: that is a result, not a reason to stop.
// Linux only, for that reason.
//
// --- Detection latency ------------------------------------------------------
//
// With a linear search every allocation revalidated every free block, so
// damage was found more or less as soon as anything happened. Binning made
// allocation O(1) and took that incidental cover away; a bounded patrol puts
// it back, and *when* it puts it back is now a tunable. So a second number
// matters alongside "was it detected": how long detection took, measured in
// allocator calls.
//
// Each trial therefore runs a stream of ordinary allocations and frees after
// the flip, deliberately never touching the damaged block, and records the
// call on which the allocator first said something was wrong. That is the
// latency the patrol governs -- a block someone reads is validated on touch
// regardless. --scrub-interval sweeps it, and the interval is a mandatory CSV
// column because results taken at different settings are not comparable.
//
// Two things to read carefully in that column. The mean is **censored**: it
// covers only the trials detected inside the window, and `latency_n` says how
// many that was. When latency_n is short of the trial count the mean is a
// lower bound, not an estimate -- which is exactly what an interval at or
// above LATENCY_OPS produces. And the latency phase runs before the
// classification phase, so the outcome buckets describe the arena after that
// window of ordinary traffic rather than at the instant of the flip.

// fork, waitpid, alarm and aligned_alloc are POSIX/C11 library features that
// strict c11 does not expose by default.
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "mars/allocator.h"
#include "mars_rng.h"
#include "mm_internal.h"

#define LIVE_BLOCKS 24
#define ARENA_DEFAULT (256u * 1024u)
#define TRIAL_TIMEOUT_SEC 5

// How many ordinary calls a trial will make while waiting for the allocator to
// notice on its own. Four times the default scrub interval, so that a patrol
// set to the default gets several chances rather than one.
#define LATENCY_OPS 4096u
#define LATENCY_NONE UINT32_MAX
#define LATENCY_SLOTS 8

// --- Outcomes ---------------------------------------------------------------

typedef enum {
  OUT_DETECTED_NO_LOSS = 0,
  OUT_DETECTED_QUARANTINED,
  OUT_DETECTED_FATAL,
  OUT_UNDETECTED_BENIGN,
  OUT_UNDETECTED_SILENT,
  OUT_CRASH,
  OUT_COUNT
} outcome_t;

static const char *outcome_name[OUT_COUNT] = {
    "detected_no_loss",   "detected_quarantined", "detected_fatal",
    "undetected_benign",  "undetected_silent",    "crash"};

// --- Injection targets ------------------------------------------------------

// The allocated-block footer is gone: with boundary tags a footer exists only
// inside a free block, so `footer` was replaced by `free_footer`. The rest are
// the same places as before, at their new offsets.
typedef enum {
  TGT_ALLOC_HDR = 0,
  TGT_FREE_HDR,
  TGT_LINKS,
  TGT_FREE_FOOTER,
  TGT_PAYLOAD,
  TGT_CANARY,
  TGT_ANY,
  TGT_COUNT
} target_t;

static const char *target_name[TGT_COUNT] = {"alloc_hdr", "free_hdr",
                                             "links",     "free_footer",
                                             "payload",   "canary",
                                             "any"};

// --- Trial ------------------------------------------------------------------

typedef struct {
  void *ptr;
  size_t size;
  uint8_t mirror[256];
} slot;

static uint8_t *g_heap;
static size_t g_heap_size = ARENA_DEFAULT;
static slot g_slots[LIVE_BLOCKS];
static int g_live;
static size_t g_scrub_interval = 1024;  // the library's own default
static size_t g_scrub_budget = 16;

// Deterministic starting state: a mixture of sizes with a few holes punched in
// it, so that both allocated and free blocks are present to aim at.
static void build_state(mars_rng *rng) {
  g_live = 0;
  if (mm_init(g_heap, g_heap_size) != 0) _exit(70);

  for (int i = 0; i < LIVE_BLOCKS; i++) {
    size_t n = 16 + (size_t)mars_rng_below(rng, 200);
    void *p = mm_malloc(n);
    if (p == NULL) break;
    slot *s = &g_slots[g_live++];
    s->ptr = p;
    s->size = n;
    for (size_t k = 0; k < n; k++) {
      s->mirror[k] = (uint8_t)(mars_rng_next(rng) & 0xFF);
    }
    if (mm_write(p, 0, s->mirror, n) != (int64_t)n) _exit(70);
  }

  // Free every third block so the arena holds a mix of live and free space.
  for (int i = g_live - 1; i >= 0; i -= 3) {
    mm_free(g_slots[i].ptr);
    g_slots[i] = g_slots[--g_live];
  }
}

// Picks a byte range to corrupt. Returns 0 if nothing suitable was found.
static int choose_region(mars_rng *rng, target_t target, uint8_t **out_base,
                         size_t *out_len) {
  if (target == TGT_ANY) {
    *out_base = g_heap;
    *out_len = g_heap_size;
    return 1;
  }
  if (target == TGT_PAYLOAD || target == TGT_CANARY) {
    if (g_live == 0) return 0;
    if (target == TGT_CANARY && MM_CANARY_SIZE == 0) {
      return 0;  // the fast profile has no canary to aim at
    }
    slot *s = &g_slots[mars_rng_below(rng, (uint64_t)g_live)];
    if (target == TGT_PAYLOAD) {
      *out_base = (uint8_t *)s->ptr;
      *out_len = s->size;
    } else {
      *out_base = (uint8_t *)s->ptr + s->size;
      *out_len = MM_CANARY_SIZE;
    }
    return 1;
  }

  // Walk the tiling -- there is no block list any more -- collecting
  // candidates of the requested kind. Free blocks are wanted for the link and
  // footer targets, since those structures exist only inside a free block.
  mm_block *candidates[256];
  size_t n = 0;
  size_t budget = mm_max_blocks();
  for (uint8_t *p = g_arena.lo; p < g_arena.hi && n < 256;) {
    if (budget-- == 0) break;
    mm_block *b = (mm_block *)(void *)p;
    if (!mm_header_ok(b)) break;
    bool want_used = (target == TGT_ALLOC_HDR);
    if (mm_is_used(b) == want_used) candidates[n++] = b;
    p += mm_block_size(b);
  }
  if (n == 0) return 0;

  mm_block *b = candidates[mars_rng_below(rng, (uint64_t)n)];
  if (target == TGT_LINKS) {
    *out_base = mm_payload_of(b);
    *out_len = 2 * sizeof(uint64_t);
  } else if (target == TGT_FREE_FOOTER) {
    *out_base = mm_block_end(b) - sizeof(uint64_t);
    *out_len = sizeof(uint64_t);
  } else {
    *out_base = (uint8_t *)(void *)b;
    *out_len = MM_HDR_SIZE;
  }
  return 1;
}

static void flip_bits(mars_rng *rng, uint8_t *base, size_t len, int bits) {
  for (int i = 0; i < bits; i++) {
    size_t byte = (size_t)mars_rng_below(rng, len);
    unsigned bit = (unsigned)mars_rng_below(rng, 8);
    base[byte] ^= (uint8_t)(1u << bit);
  }
}

// Ordinary allocator traffic that deliberately avoids the live set, run until
// the allocator reports something wrong. Returns the call it happened on, or
// LATENCY_NONE if it never did within LATENCY_OPS.
//
// Nothing here reads or writes a block that was corrupted, so validate-on-
// touch cannot be what finds the damage: whatever this measures, the patrol
// is what governs it. Scratch blocks are freed as it goes, so the heap the
// classification phase then inspects is the one the flip left behind.
static uint32_t measure_latency(mars_rng *rng) {
  void *scratch[LATENCY_SLOTS] = {0};
  uint32_t at = LATENCY_NONE;

  for (uint32_t op = 0; op < LATENCY_OPS; op++) {
    size_t k = (size_t)mars_rng_below(rng, LATENCY_SLOTS);
    if (scratch[k] != NULL) {
      mm_free(scratch[k]);
      scratch[k] = NULL;
    } else {
      scratch[k] = mm_malloc(16 + (size_t)mars_rng_below(rng, 240));
    }
    if (mm_last_error() != MM_OK) {
      at = op;
      break;
    }
  }

  for (int i = 0; i < LATENCY_SLOTS; i++) {
    if (scratch[i] != NULL) mm_free(scratch[i]);
  }
  return at;
}

// Runs one trial to completion and exits with the outcome, having written the
// detection latency to `fd` first. Never returns.
static void run_child(uint64_t seed, target_t target, int bits, int fd) {
  mars_rng rng;
  mars_rng_seed(&rng, seed);

  // The arena secret is normally drawn from the clock and a stack address,
  // which would make a trial unrepeatable. Pin it to the trial seed so a
  // result found here can be replayed byte for byte.
  mm_pin_secret(seed | 1u);

  // Off while the starting state is built, so that setting up an undamaged
  // arena neither costs patrol budget nor advances the counter the latency is
  // measured from. It goes on immediately before the flip.
  mm_set_scrub_interval(0, 0);
  build_state(&rng);

  uint8_t *base = NULL;
  size_t len = 0;
  if (!choose_region(&rng, target, &base, &len) || len == 0) {
    _exit(71);  // nothing of that kind existed; parent discards the trial
  }
  mm_set_scrub_interval(g_scrub_interval, g_scrub_budget);
  flip_bits(&rng, base, len, bits);

  uint32_t latency = measure_latency(&rng);
  ssize_t ignored = write(fd, &latency, sizeof(latency));
  (void)ignored;
  close(fd);

  // Noticing during the latency phase counts: it is the allocator reporting a
  // problem, which is all "detected" has ever meant here.
  int detected = (latency != LATENCY_NONE);
  int mismatch = 0;   // data came back wrong
  int silent = 0;     // data came back wrong AND the call claimed success

  for (int i = 0; i < g_live; i++) {
    slot *s = &g_slots[i];

    if (mm_verify(s->ptr) != MM_OK) detected = 1;

    uint8_t back[256];
    int64_t got = mm_read(s->ptr, 0, back, s->size);
    if (got != (int64_t)s->size) {
      detected = 1;
      continue;
    }
    // The read said it succeeded. If the bytes differ, nothing warned us.
    if (memcmp(back, s->mirror, s->size) != 0) {
      mismatch = 1;
      silent = 1;
    }
  }

  if (mm_check_heap() != MM_OK) detected = 1;

  for (int i = 0; i < g_live; i++) {
    mm_free(g_slots[i].ptr);
    if (mm_last_error() != MM_OK) detected = 1;
  }

  // "Usable" means the allocator still serves requests. Whether the heap
  // check is happy is a separate question: quarantine deliberately abandons
  // space, and an allocator that keeps working after losing a block is a very
  // different outcome from one that cannot allocate at all.
  int usable = (mm_malloc(64) != NULL);

  // Ask the arena what it gave up, not the counters: space abandoned by
  // truncating the list is lost just as surely as a quarantined block, and
  // counting only quarantines would report a heap that shed most of itself as
  // having lost nothing.
  size_t lost = g_arena.lost_bytes;

  if (silent) _exit(OUT_UNDETECTED_SILENT);
  if (!detected && !mismatch) _exit(OUT_UNDETECTED_BENIGN);
  if (!usable) _exit(OUT_DETECTED_FATAL);
  // Deliberately not called "recovered": nothing here repairs a block. This
  // bucket means the damage was noticed and cost nothing -- the allocator kept
  // working and surrendered no memory. A genuine repair outcome only becomes
  // reachable once a profile carries enough redundancy to rebuild a header.
  if (lost > 0) _exit(OUT_DETECTED_QUARANTINED);
  _exit(OUT_DETECTED_NO_LOSS);
}

// --- Provenance -------------------------------------------------------------

// Written as `#` comment lines above the CSV header, for the same reason the
// benchmark writes them: a detection rate whose machine, compiler, profile and
// commit are unknown cannot be compared with another one or reproduced. Anything
// reading these files has to skip lines beginning with `#`.
static void write_env_comment(FILE *out, const char *git_sha, uint64_t trials,
                              uint64_t base_seed, size_t arena) {
  char cpu[256] = "unknown";
  FILE *f = fopen("/proc/cpuinfo", "r");
  if (f != NULL) {
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
      if (strncmp(line, "model name", 10) != 0) continue;
      char *colon = strchr(line, ':');
      if (colon != NULL) {
        char *v = colon + 1;
        while (*v == ' ') v++;
        size_t len = strlen(v);
        while (len > 0 && (v[len - 1] == '\n' || v[len - 1] == ' ')) len--;
        snprintf(cpu, sizeof(cpu), "%.*s", (int)len, v);
      }
      break;
    }
    fclose(f);
  }

  char kernel[256] = "unknown";
  struct utsname u;
  if (uname(&u) == 0) snprintf(kernel, sizeof(kernel), "%s %s", u.sysname, u.release);

  time_t now = time(NULL);
  char when[64];
  strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));

  fprintf(out, "# cpu=%s\n", cpu);
  fprintf(out, "# kernel=%s\n", kernel);
  fprintf(out, "# compiler=%s\n",
#if defined(__clang__)
          "clang " __clang_version__
#elif defined(__GNUC__)
          "gcc " __VERSION__
#else
          "unknown"
#endif
  );
  fprintf(out, "# date=%s\n", when);
  fprintf(out, "# git_sha=%s\n", git_sha);
  fprintf(out, "# profile=%s metadata_bytes=%zu\n", mm_profile(),
          mm_metadata_overhead());
  fprintf(out, "# trials_per_cell=%llu base_seed=%llu arena_bytes=%zu\n",
          (unsigned long long)trials, (unsigned long long)base_seed, arena);
  // The scrub interval is deliberately not recorded here. A sweep appends
  // several intervals into one file and the header is written once, so a
  // header field would describe only whichever run created the file. It is a
  // per-row column for exactly that reason.
  fprintf(out, "# latency_window_ops=%u\n", LATENCY_OPS);
}

// --- Statistics -------------------------------------------------------------

// Wilson score interval: behaves sensibly at proportions near 0 and 1, where
// the textbook normal approximation produces bounds outside [0,1].
static void wilson(uint64_t successes, uint64_t total, double *lo, double *hi) {
  if (total == 0) {
    *lo = *hi = 0.0;
    return;
  }
  const double z = 1.959964;
  double n = (double)total;
  double p = (double)successes / n;
  double denom = 1.0 + z * z / n;
  double centre = (p + z * z / (2 * n)) / denom;
  double half = z * sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom;
  *lo = (centre - half) * 100.0;
  *hi = (centre + half) * 100.0;
  if (*lo < 0.0) *lo = 0.0;
  if (*hi > 100.0) *hi = 100.0;
}

// --- Driver -----------------------------------------------------------------

static void usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("  --trials N     trials per configuration (default 2000)\n");
  printf("  --seed N       base seed (default 20260809)\n");
  printf("  --bits LIST    comma-separated flip counts (default 1,2,4,8)\n");
  printf("  --target NAME  restrict to one target (default: all)\n");
  printf("  --arena N      arena bytes (default %u)\n", ARENA_DEFAULT);
  printf("  --scrub-interval N|off  calls between patrols (default 1024)\n");
  printf("  --scrub-budget N        blocks per patrol (default 16)\n");
  printf("  --csv FILE     machine-readable table; appended to if it exists,\n");
  printf("                 so a sweep can accumulate into one file\n");
  printf("  --git-sha SHA  recorded in the CSV provenance header\n");
  printf("  --targets      list targets and exit\n");
}

int main(int argc, char **argv) {
  uint64_t trials = 2000;
  uint64_t base_seed = 20260809;
  int bits_list[8] = {1, 2, 4, 8};
  int bits_count = 4;
  const char *only_target = NULL;
  const char *csv_path = NULL;
  const char *git_sha = "unknown";

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--trials") && i + 1 < argc) trials = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--seed") && i + 1 < argc) base_seed = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--arena") && i + 1 < argc) g_heap_size = (size_t)strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--target") && i + 1 < argc) only_target = argv[++i];
    else if (!strcmp(a, "--csv") && i + 1 < argc) csv_path = argv[++i];
    else if (!strcmp(a, "--git-sha") && i + 1 < argc) git_sha = argv[++i];
    else if (!strcmp(a, "--scrub-budget") && i + 1 < argc) g_scrub_budget = (size_t)strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--scrub-interval") && i + 1 < argc) {
      // "off" and 0 are the same thing -- no automatic patrol at all -- and
      // both are spelled out because a sweep reads better with a word than
      // with a zero that could be mistaken for "as often as possible".
      const char *v = argv[++i];
      g_scrub_interval = strcmp(v, "off") == 0 ? 0 : (size_t)strtoull(v, NULL, 10);
    }
    else if (!strcmp(a, "--bits") && i + 1 < argc) {
      bits_count = 0;
      char *spec = argv[++i];
      for (char *tok = strtok(spec, ","); tok && bits_count < 8;
           tok = strtok(NULL, ",")) {
        bits_list[bits_count++] = atoi(tok);
      }
    } else if (!strcmp(a, "--targets")) {
      for (int t = 0; t < TGT_COUNT; t++) printf("%s\n", target_name[t]);
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

  g_heap = (uint8_t *)aligned_alloc(64, g_heap_size);
  if (g_heap == NULL) {
    fprintf(stderr, "could not allocate a %zu byte arena\n", g_heap_size);
    return 2;
  }

  // Appended to rather than truncated, and the header written only when the
  // file is created, so that a sweep over several scrub intervals accumulates
  // into one table instead of each run erasing the last.
  FILE *csv = NULL;
  if (csv_path != NULL) {
    FILE *probe = fopen(csv_path, "r");
    bool fresh = (probe == NULL);
    if (probe != NULL) fclose(probe);

    csv = fopen(csv_path, "a");
    if (csv != NULL && fresh) {
      write_env_comment(csv, git_sha, trials, base_seed, g_heap_size);
      fprintf(csv, "profile,scrub_interval,scrub_budget,target,bits,trials,"
                   "discarded");
      for (int o = 0; o < OUT_COUNT; o++) fprintf(csv, ",%s", outcome_name[o]);
      fprintf(csv, ",detection_pct,detection_lo,detection_hi,"
                   "silent_pct,silent_lo,silent_hi,"
                   "latency_n,latency_mean_ops,latency_max_ops\n");
    }
  }

  // The profile decides how much metadata a block carries, so it decides what
  // can be detected. A results table without it on the front is not comparable
  // with any other results table.
  printf("fault injection: profile %s, %zu B metadata per block\n",
         mm_profile(), mm_metadata_overhead());
  printf("%llu trials per cell, seed %llu, arena %zu bytes\n",
         (unsigned long long)trials, (unsigned long long)base_seed,
         g_heap_size);
  // The scrub interval decides how long damage nobody touches can sit there
  // unnoticed, so a detection rate quoted without it is not a comparable
  // number. It goes on the front of the run and into every CSV row.
  if (g_scrub_interval == 0) {
    printf("patrol off; latency measured over %u calls per trial\n",
           LATENCY_OPS);
  } else {
    printf("patrol every %zu calls, %zu blocks each; latency measured over "
           "%u calls per trial\n",
           g_scrub_interval, g_scrub_budget, LATENCY_OPS);
  }
  printf("%-10s %5s %8s %8s %8s %8s %8s %8s   %-18s %-18s %11s\n", "target",
         "bits", "recov", "quaran", "fatal", "benign", "SILENT", "crash",
         "detection% (95% CI)", "silent% (95% CI)", "latency ops");

  for (int t = 0; t < TGT_COUNT; t++) {
    if (only_target != NULL && strcmp(only_target, target_name[t]) != 0) continue;

    for (int b = 0; b < bits_count; b++) {
      uint64_t counts[OUT_COUNT] = {0};
      uint64_t discarded = 0;
      uint64_t latency_n = 0;
      uint64_t latency_sum = 0;
      uint32_t latency_max = 0;

      for (uint64_t trial = 0; trial < trials; trial++) {
        uint64_t seed = base_seed + trial * 1000003ull + (uint64_t)t * 97ull +
                        (uint64_t)b;
        fflush(stdout);

        // The outcome comes back as an exit code, which leaves no room for a
        // second number. The latency comes back through a pipe instead; a
        // child that crashed writes nothing, and there is no latency to
        // record for a trial that never finished.
        int fds[2];
        if (pipe(fds) != 0) {
          perror("pipe");
          return 2;
        }

        pid_t pid = fork();
        if (pid < 0) {
          perror("fork");
          return 2;
        }
        if (pid == 0) {
          close(fds[0]);
          alarm(TRIAL_TIMEOUT_SEC);
          run_child(seed, (target_t)t, bits_list[b], fds[1]);
          _exit(72);  // unreachable
        }
        close(fds[1]);

        uint32_t latency = LATENCY_NONE;
        if (read(fds[0], &latency, sizeof(latency)) != (ssize_t)sizeof(latency)) {
          latency = LATENCY_NONE;
        }
        close(fds[0]);

        int status = 0;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) < OUT_COUNT &&
            latency != LATENCY_NONE) {
          latency_n++;
          latency_sum += latency;
          if (latency > latency_max) latency_max = latency;
        }

        if (WIFEXITED(status)) {
          int code = WEXITSTATUS(status);
          if (code < OUT_COUNT) {
            counts[code]++;
          } else {
            discarded++;  // setup failed or no such region existed
          }
        } else {
          // Killed by a signal, alarm included: the allocator went somewhere
          // it should not have.
          counts[OUT_CRASH]++;
        }
      }

      uint64_t counted = 0;
      for (int o = 0; o < OUT_COUNT; o++) counted += counts[o];
      if (counted == 0) continue;

      uint64_t detected = counts[OUT_DETECTED_NO_LOSS] +
                          counts[OUT_DETECTED_QUARANTINED] +
                          counts[OUT_DETECTED_FATAL];
      // Coverage: of the flips that mattered, how many were caught. Benign
      // flips are excluded -- there was nothing to catch.
      uint64_t mattered = detected + counts[OUT_UNDETECTED_SILENT];
      double dlo, dhi, slo, shi;
      wilson(detected, mattered == 0 ? 1 : mattered, &dlo, &dhi);
      wilson(counts[OUT_UNDETECTED_SILENT], counted, &slo, &shi);

      double detection_pct =
          mattered == 0 ? 100.0 : 100.0 * (double)detected / (double)mattered;
      double silent_pct =
          100.0 * (double)counts[OUT_UNDETECTED_SILENT] / (double)counted;

      // Averaged over the trials that were detected without anyone touching
      // the damage. Trials where nothing was ever noticed are excluded rather
      // than counted as LATENCY_OPS, which would be a made-up number.
      double latency_mean =
          latency_n == 0 ? 0.0 : (double)latency_sum / (double)latency_n;

      printf("%-10s %5d %8llu %8llu %8llu %8llu %8llu %8llu   "
             "%6.2f [%5.2f,%6.2f]  %6.2f [%5.2f,%6.2f] %11.1f\n",
             target_name[t], bits_list[b],
             (unsigned long long)counts[OUT_DETECTED_NO_LOSS],
             (unsigned long long)counts[OUT_DETECTED_QUARANTINED],
             (unsigned long long)counts[OUT_DETECTED_FATAL],
             (unsigned long long)counts[OUT_UNDETECTED_BENIGN],
             (unsigned long long)counts[OUT_UNDETECTED_SILENT],
             (unsigned long long)counts[OUT_CRASH], detection_pct, dlo, dhi,
             silent_pct, slo, shi, latency_mean);
      fflush(stdout);

      if (csv != NULL) {
        fprintf(csv, "%s,%zu,%zu,%s,%d,%llu,%llu", mm_profile(),
                g_scrub_interval, g_scrub_budget, target_name[t], bits_list[b],
                (unsigned long long)counted, (unsigned long long)discarded);
        for (int o = 0; o < OUT_COUNT; o++) {
          fprintf(csv, ",%llu", (unsigned long long)counts[o]);
        }
        fprintf(csv, ",%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%llu,%.4f,%u\n",
                detection_pct, dlo, dhi, silent_pct, slo, shi,
                (unsigned long long)latency_n, latency_mean, latency_max);
        fflush(csv);
      }
    }
  }

  if (csv != NULL) fclose(csv);
  free(g_heap);
  return 0;
}
