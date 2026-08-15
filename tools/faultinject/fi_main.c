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

// fork, waitpid, alarm and aligned_alloc are POSIX/C11 library features that
// strict c11 does not expose by default.
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mars/allocator.h"
#include "mars_rng.h"
#include "mm_internal.h"

#define LIVE_BLOCKS 24
#define ARENA_DEFAULT (256u * 1024u)
#define TRIAL_TIMEOUT_SEC 5

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

// Runs one trial to completion and exits with the outcome. Never returns.
static void run_child(uint64_t seed, target_t target, int bits) {
  mars_rng rng;
  mars_rng_seed(&rng, seed);

  // The arena secret is normally drawn from the clock and a stack address,
  // which would make a trial unrepeatable. Pin it to the trial seed so a
  // result found here can be replayed byte for byte.
  mm_pin_secret(seed | 1u);

  build_state(&rng);

  uint8_t *base = NULL;
  size_t len = 0;
  if (!choose_region(&rng, target, &base, &len) || len == 0) {
    _exit(71);  // nothing of that kind existed; parent discards the trial
  }
  flip_bits(&rng, base, len, bits);

  int detected = 0;   // the allocator reported a problem at some point
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
  printf("  --csv FILE     append a machine-readable table here\n");
  printf("  --targets      list targets and exit\n");
}

int main(int argc, char **argv) {
  uint64_t trials = 2000;
  uint64_t base_seed = 20260809;
  int bits_list[8] = {1, 2, 4, 8};
  int bits_count = 4;
  const char *only_target = NULL;
  const char *csv_path = NULL;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if (!strcmp(a, "--trials") && i + 1 < argc) trials = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--seed") && i + 1 < argc) base_seed = strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--arena") && i + 1 < argc) g_heap_size = (size_t)strtoull(argv[++i], NULL, 10);
    else if (!strcmp(a, "--target") && i + 1 < argc) only_target = argv[++i];
    else if (!strcmp(a, "--csv") && i + 1 < argc) csv_path = argv[++i];
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

  FILE *csv = NULL;
  if (csv_path != NULL) {
    csv = fopen(csv_path, "w");
    if (csv != NULL) {
      fprintf(csv, "profile,target,bits,trials,discarded");
      for (int o = 0; o < OUT_COUNT; o++) fprintf(csv, ",%s", outcome_name[o]);
      fprintf(csv, ",detection_pct,detection_lo,detection_hi,"
                   "silent_pct,silent_lo,silent_hi\n");
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
  printf("%-10s %5s %8s %8s %8s %8s %8s %8s   %-18s %-18s\n", "target", "bits",
         "recov", "quaran", "fatal", "benign", "SILENT", "crash",
         "detection% (95% CI)", "silent% (95% CI)");

  for (int t = 0; t < TGT_COUNT; t++) {
    if (only_target != NULL && strcmp(only_target, target_name[t]) != 0) continue;

    for (int b = 0; b < bits_count; b++) {
      uint64_t counts[OUT_COUNT] = {0};
      uint64_t discarded = 0;

      for (uint64_t trial = 0; trial < trials; trial++) {
        uint64_t seed = base_seed + trial * 1000003ull + (uint64_t)t * 97ull +
                        (uint64_t)b;
        fflush(stdout);
        pid_t pid = fork();
        if (pid < 0) {
          perror("fork");
          return 2;
        }
        if (pid == 0) {
          alarm(TRIAL_TIMEOUT_SEC);
          run_child(seed, (target_t)t, bits_list[b]);
          _exit(72);  // unreachable
        }

        int status = 0;
        waitpid(pid, &status, 0);

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

      printf("%-10s %5d %8llu %8llu %8llu %8llu %8llu %8llu   "
             "%6.2f [%5.2f,%6.2f]  %6.2f [%5.2f,%6.2f]\n",
             target_name[t], bits_list[b],
             (unsigned long long)counts[OUT_DETECTED_NO_LOSS],
             (unsigned long long)counts[OUT_DETECTED_QUARANTINED],
             (unsigned long long)counts[OUT_DETECTED_FATAL],
             (unsigned long long)counts[OUT_UNDETECTED_BENIGN],
             (unsigned long long)counts[OUT_UNDETECTED_SILENT],
             (unsigned long long)counts[OUT_CRASH], detection_pct, dlo, dhi,
             silent_pct, slo, shi);
      fflush(stdout);

      if (csv != NULL) {
        fprintf(csv, "%s,%s,%d,%llu,%llu", mm_profile(), target_name[t],
                bits_list[b], (unsigned long long)counted,
                (unsigned long long)discarded);
        for (int o = 0; o < OUT_COUNT; o++) {
          fprintf(csv, ",%llu", (unsigned long long)counts[o]);
        }
        fprintf(csv, ",%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", detection_pct, dlo,
                dhi, silent_pct, slo, shi);
        fflush(csv);
      }
    }
  }

  if (csv != NULL) fclose(csv);
  free(g_heap);
  return 0;
}
