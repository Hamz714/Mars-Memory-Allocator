// Differential fuzzer.
//
// Drives the allocator with a seeded stream of operations while maintaining a
// shadow model of what every live block is supposed to contain. After each
// operation the allocator's view and the model's view must agree; any
// divergence is a bug in the allocator, not in the model.
//
// Every run is reproducible from its seed, so a failure reports the seed and
// the operation index, and --replay re-runs exactly that stream.
//
//   fuzz_shadow --seconds 60
//   fuzz_shadow --seed 12345 --ops 200000
//   fuzz_shadow --replay 12345 --stop-at 8317 --verbose

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "mars/allocator.h"
#include "mars_rng.h"

#define MAX_LIVE 192
#define DEFAULT_ARENA (256u * 1024u)

typedef struct {
  void *ptr;        // what the allocator handed back
  size_t size;      // its requested size
  uint8_t *mirror;  // what the payload is supposed to contain
  uint64_t id;      // distinguishes blocks in diagnostics
} slot;

static slot g_live[MAX_LIVE];
static int g_count;
static uint64_t g_next_id = 1;

static uint64_t g_seed;
static uint64_t g_op;       // index of the operation in flight
static int g_verbose;
static uint8_t *g_arena_mem;
static size_t g_arena_size = DEFAULT_ARENA;

// --- Failure reporting -----------------------------------------------------

static void fail(const char *what, const char *detail) {
  fprintf(stderr,
          "\nFUZZ FAILURE\n"
          "  %s%s%s\n"
          "  seed        %" PRIu64 "\n"
          "  operation   %" PRIu64 "\n"
          "  live blocks %d\n"
          "  arena       %zu bytes\n"
          "  last status %s\n"
          "\n"
          "  replay with: fuzz_shadow --replay %" PRIu64
          " --stop-at %" PRIu64 " --verbose\n",
          what, detail ? ": " : "", detail ? detail : "", g_seed, g_op,
          g_count, g_arena_size, mm_strerror(mm_last_error()), g_seed, g_op);
  exit(1);
}

// --- Shadow bookkeeping ----------------------------------------------------

// Contents a block is given when it is created or extended. Derived from the
// block id and offset, so any byte landing in the wrong block is visible.
static uint8_t expected_byte(uint64_t id, size_t offset) {
  return (uint8_t)(id * 131u + offset * 17u + 0x5Au);
}

static void paint(slot *s, size_t from, size_t to) {
  for (size_t i = from; i < to; i++) {
    s->mirror[i] = expected_byte(s->id, i);
  }
  if (to > from) {
    if (mm_write(s->ptr, from, s->mirror + from, to - from) !=
        (int64_t)(to - from)) {
      fail("mm_write of freshly owned bytes failed", mm_strerror(mm_last_error()));
    }
  }
}

// Reads a block back in full and compares it against the model.
static void compare(slot *s) {
  if (s->size == 0) return;
  uint8_t *back = (uint8_t *)malloc(s->size);
  if (back == NULL) return;
  int64_t got = mm_read(s->ptr, 0, back, s->size);
  if (got != (int64_t)s->size) {
    free(back);
    fail("mm_read of a live block failed", mm_strerror(mm_last_error()));
  }
  for (size_t i = 0; i < s->size; i++) {
    if (back[i] != s->mirror[i]) {
      char detail[160];
      snprintf(detail, sizeof(detail),
               "block id %" PRIu64 " size %zu: byte %zu is %u, model says %u",
               s->id, s->size, i, back[i], s->mirror[i]);
      free(back);
      fail("payload diverged from the model", detail);
    }
  }
  free(back);
}

// --- Invariants ------------------------------------------------------------

static void check_invariants(int deep) {
  if (mm_check_heap() != MM_OK) {
    fail("mm_check_heap reported an inconsistent arena",
         mm_strerror(mm_last_error()));
  }

  for (int i = 0; i < g_count; i++) {
    if ((uintptr_t)g_live[i].ptr % mm_alignment() != 0) {
      fail("a live pointer is not aligned", NULL);
    }
    if (mm_verify(g_live[i].ptr) != MM_OK) {
      fail("mm_verify rejected a live block", mm_strerror(mm_last_error()));
    }
  }

  if (!deep) return;

  // No two live blocks may share storage.
  for (int i = 0; i < g_count; i++) {
    uintptr_t ai = (uintptr_t)g_live[i].ptr;
    uintptr_t aend = ai + g_live[i].size;
    for (int j = i + 1; j < g_count; j++) {
      uintptr_t bi = (uintptr_t)g_live[j].ptr;
      uintptr_t bend = bi + g_live[j].size;
      if (ai < bend && bi < aend) {
        fail("two live blocks overlap", NULL);
      }
    }
  }
  for (int i = 0; i < g_count; i++) compare(&g_live[i]);
}

// --- Operations ------------------------------------------------------------

// Small sizes dominate, with an occasional large request.
static size_t draw_size(mars_rng *r) {
  uint64_t roll = mars_rng_below(r, 100);
  if (roll < 70) return 1 + mars_rng_below(r, 128);
  if (roll < 95) return 1 + mars_rng_below(r, 2048);
  return 1 + mars_rng_below(r, 16384);
}

static void do_malloc(mars_rng *r) {
  if (g_count >= MAX_LIVE) return;
  size_t n = draw_size(r);
  void *p = mm_malloc(n);
  if (p == NULL) return;  // exhaustion is a legitimate answer

  slot *s = &g_live[g_count++];
  s->ptr = p;
  s->size = n;
  s->id = g_next_id++;
  s->mirror = (uint8_t *)malloc(n);
  if (s->mirror == NULL) fail("out of host memory for the model", NULL);

  // A fresh block's contents are unspecified, so establish them before the
  // model can claim to know anything about them.
  paint(s, 0, n);
  if (g_verbose) printf("%" PRIu64 ": malloc(%zu) -> id %" PRIu64 "\n", g_op, n, s->id);
}

static void do_free(mars_rng *r) {
  if (g_count == 0) return;
  int i = (int)mars_rng_below(r, (uint64_t)g_count);
  slot *s = &g_live[i];
  if (g_verbose) printf("%" PRIu64 ": free(id %" PRIu64 ")\n", g_op, s->id);

  compare(s);  // last look before it goes
  mm_free(s->ptr);
  free(s->mirror);
  g_live[i] = g_live[--g_count];
}

static void do_realloc(mars_rng *r) {
  if (g_count == 0) return;
  int i = (int)mars_rng_below(r, (uint64_t)g_count);
  slot *s = &g_live[i];
  size_t want = draw_size(r);
  size_t old = s->size;

  if (g_verbose) {
    printf("%" PRIu64 ": realloc(id %" PRIu64 ", %zu -> %zu)\n", g_op, s->id,
           old, want);
  }

  void *p = mm_realloc(s->ptr, want);
  if (p == NULL) {
    // A failed resize must leave the original block exactly as it was.
    compare(s);
    return;
  }

  uint8_t *grown = (uint8_t *)realloc(s->mirror, want);
  if (grown == NULL) fail("out of host memory for the model", NULL);
  s->mirror = grown;
  s->ptr = p;
  s->size = want;

  // Bytes beyond the old size are unspecified after a grow, so the model takes
  // ownership of them the same way it does for a fresh block.
  if (want > old) paint(s, old, want);
  compare(s);
}

static void do_write(mars_rng *r) {
  if (g_count == 0) return;
  slot *s = &g_live[mars_rng_below(r, (uint64_t)g_count)];
  size_t off = mars_rng_below(r, s->size);
  size_t len = 1 + mars_rng_below(r, s->size - off);

  for (size_t i = 0; i < len; i++) {
    s->mirror[off + i] = (uint8_t)mars_rng_next(r);
  }
  if (mm_write(s->ptr, off, s->mirror + off, len) != (int64_t)len) {
    fail("mm_write of an in-bounds range failed", mm_strerror(mm_last_error()));
  }
}

static void do_read(mars_rng *r) {
  if (g_count == 0) return;
  slot *s = &g_live[mars_rng_below(r, (uint64_t)g_count)];
  size_t off = mars_rng_below(r, s->size);
  size_t len = 1 + mars_rng_below(r, s->size - off);

  uint8_t *back = (uint8_t *)malloc(len);
  if (back == NULL) return;
  if (mm_read(s->ptr, off, back, len) != (int64_t)len) {
    free(back);
    fail("mm_read of an in-bounds range failed", mm_strerror(mm_last_error()));
  }
  if (memcmp(back, s->mirror + off, len) != 0) {
    free(back);
    fail("a partial read diverged from the model", NULL);
  }
  free(back);
}

// Out-of-range arguments must be refused rather than acted on.
static void do_abuse(mars_rng *r) {
  if (g_count == 0) return;
  slot *s = &g_live[mars_rng_below(r, (uint64_t)g_count)];
  uint8_t buf[64];

  if (mm_read(s->ptr, s->size + 1, buf, 1) != -1) fail("read past the end was accepted", NULL);
  if (mm_read(s->ptr, 0, buf, s->size + 1) != -1) fail("oversized read was accepted", NULL);
  if (mm_read(s->ptr, SIZE_MAX - 3, buf, 8) != -1) fail("wrapping read offset was accepted", NULL);
  if (mm_write(s->ptr, SIZE_MAX - 3, buf, 8) != -1) fail("wrapping write offset was accepted", NULL);

  // Pointers the arena never handed out.
  if (mm_read(buf, 0, buf, 1) != -1) fail("read through a foreign pointer was accepted", NULL);
  mm_free(buf);
}

// --- Driver ----------------------------------------------------------------

static void reset_arena(void) {
  for (int i = 0; i < g_count; i++) free(g_live[i].mirror);
  g_count = 0;
  if (mm_init(g_arena_mem, g_arena_size) != 0) {
    fail("mm_init rejected the arena", mm_strerror(mm_last_error()));
  }
}

static void usage(const char *argv0) {
  printf("Usage: %s [options]\n", argv0);
  printf("  --seed N       seed the operation stream (default: time-based)\n");
  printf("  --replay N     alias for --seed, reads better when reproducing\n");
  printf("  --ops N        run N operations (default 200000)\n");
  printf("  --seconds N    stop after N seconds instead of a fixed count\n");
  printf("  --stop-at N    halt just after operation N\n");
  printf("  --arena N      arena size in bytes (default %u)\n", DEFAULT_ARENA);
  printf("  --verbose      log every operation\n");
}

int main(int argc, char **argv) {
  uint64_t ops = 200000;
  uint64_t stop_at = 0;
  double seconds = 0.0;
  int seeded = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];
    if ((!strcmp(a, "--seed") || !strcmp(a, "--replay")) && i + 1 < argc) {
      g_seed = strtoull(argv[++i], NULL, 10);
      seeded = 1;
    } else if (!strcmp(a, "--ops") && i + 1 < argc) {
      ops = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--seconds") && i + 1 < argc) {
      seconds = strtod(argv[++i], NULL);
    } else if (!strcmp(a, "--stop-at") && i + 1 < argc) {
      stop_at = strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--arena") && i + 1 < argc) {
      g_arena_size = (size_t)strtoull(argv[++i], NULL, 10);
    } else if (!strcmp(a, "--verbose")) {
      g_verbose = 1;
    } else if (!strcmp(a, "--help")) {
      usage(argv[0]);
      return 0;
    } else {
      fprintf(stderr, "unknown argument: %s\n", a);
      usage(argv[0]);
      return 2;
    }
  }

  if (!seeded) g_seed = (uint64_t)time(NULL);

  g_arena_mem = (uint8_t *)malloc(g_arena_size);
  if (g_arena_mem == NULL) {
    fprintf(stderr, "could not allocate a %zu byte arena\n", g_arena_size);
    return 2;
  }

  mars_rng rng;
  mars_rng_seed(&rng, g_seed);
  reset_arena();

  printf("fuzzing: seed %" PRIu64 ", arena %zu bytes\n", g_seed, g_arena_size);

  clock_t start = clock();
  uint64_t performed = 0;

  for (g_op = 1;; g_op++) {
    if (seconds > 0.0) {
      if ((double)(clock() - start) / CLOCKS_PER_SEC >= seconds) break;
    } else if (g_op > ops) {
      break;
    }
    if (stop_at != 0 && g_op > stop_at) break;

    uint64_t roll = mars_rng_below(&rng, 100);
    if (roll < 28) {
      do_malloc(&rng);
    } else if (roll < 51) {
      do_free(&rng);
    } else if (roll < 63) {
      do_realloc(&rng);
    } else if (roll < 81) {
      do_write(&rng);
    } else if (roll < 94) {
      do_read(&rng);
    } else if (roll < 97) {
      do_abuse(&rng);
    } else {
      // Occasionally tear the arena down and start again, which exercises
      // initialisation against a heap that is already full of blocks.
      reset_arena();
    }
    performed++;

    // The cheap invariants run every operation; the quadratic ones periodically.
    check_invariants((g_op % 512) == 0);
  }

  check_invariants(1);
  for (int i = 0; i < g_count; i++) {
    compare(&g_live[i]);
    mm_free(g_live[i].ptr);
    free(g_live[i].mirror);
  }
  g_count = 0;
  if (mm_check_heap() != MM_OK) {
    fail("arena inconsistent after releasing everything",
         mm_strerror(mm_last_error()));
  }

  free(g_arena_mem);
  printf("ok: %" PRIu64 " operations, seed %" PRIu64 "\n", performed, g_seed);
  return 0;
}
