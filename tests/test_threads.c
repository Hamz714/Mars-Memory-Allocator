// The allocator under threads.
//
// Every case here is written so that it would still be meaningful if the
// locking strategy changed underneath it: nothing tests a mutex, and nothing
// reaches into the arena. What they test is the four things a threaded program
// is entitled to -- that concurrent allocation leaves a consistent heap, that a
// block can be freed by a thread other than the one that allocated it, that a
// thread exiting does not take live memory with it, and that a random mix of
// operations across threads leaves the arena walkable.
//
// Under `MARS_LOCK=none` there is no locking to test and running these would be
// a deliberate data race, so they do not run. The first case checks that the
// build says which strategy it has rather than leaving a reader to infer it
// from which tests were skipped.

#include "mars_test.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mars_rng.h"
#include "mm_internal.h"

#define ARENA_SIZE (32u * 1024u * 1024u)
#define THREADS 4

// Enough work to interleave on a real machine, and little enough that the file
// still finishes inside ctest's timeout under valgrind, where every memory
// access costs a hundred times what it does here.
#define OPS 1500
#define LIVE 96

static uint8_t *g_heap;

// --- What the build was configured to do -----------------------------------

MM_TEST(threads, the_build_says_which_locking_strategy_it_has) {
#if MM_LOCK == MM_LOCK_NONE
  CHECK_STR_EQ(mm_lock_strategy(), "none");
#else
  CHECK_STR_EQ(mm_lock_strategy(), "global");
#endif
  CHECK_EQ(mm_arena_count(), 1u);
}

#if MM_LOCK != MM_LOCK_NONE

// One failure counter per worker. The CHECK_ macros write through a pointer the
// test body owns, so a worker cannot use them: it records here, and the body
// folds the results in after the join.
typedef struct worker {
  pthread_t id;
  unsigned index;
  uint64_t seed;
  int failures;
  const char *first_failure;
  uint64_t completed;
} worker;

static void note(worker *w, const char *what) {
  w->failures++;
  if (w->first_failure == NULL) w->first_failure = what;
}

// Sizes with the same shape as the benchmark's: mostly small, occasionally a
// kilobyte or two. Small enough that THREADS x LIVE of them fit the arena
// many times over, so a NULL is a defect here rather than exhaustion.
static size_t draw_size(mars_rng *r) {
  return mars_rng_below(r, 10) == 0 ? 512 + mars_rng_below(r, 1536)
                                    : 1 + mars_rng_below(r, 200);
}

#define MAX_DRAW 2048

static void fill(uint8_t *buf, size_t n, unsigned seed) {
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(seed + i);
}

// Starts `n` workers on `fn`, joins them, and folds each one's failures into
// the test body's counter.
static void run_workers(int *mm_failures_, worker *w, unsigned n,
                        void *(*fn)(void *)) {
  for (unsigned i = 0; i < n; i++) {
    w[i].index = i;
    w[i].seed = mars_test_seed() + 0x9E3779B97F4A7C15ULL * (i + 1);
    w[i].failures = 0;
    w[i].first_failure = NULL;
    w[i].completed = 0;
    if (pthread_create(&w[i].id, NULL, fn, &w[i]) != 0) {
      MARS_FAIL_("could not create worker %u", i);
      // Join whatever did start, so a partial failure does not leave threads
      // running on into the next test.
      for (unsigned j = 0; j < i; j++) pthread_join(w[j].id, NULL);
      return;
    }
  }
  for (unsigned i = 0; i < n; i++) {
    pthread_join(w[i].id, NULL);
    if (w[i].failures != 0) {
      MARS_FAIL_("worker %u (seed %" PRIu64 "): %d failures, first was %s", i,
                 w[i].seed, w[i].failures,
                 w[i].first_failure != NULL ? w[i].first_failure : "?");
    }
  }
}

// --- Each thread minding its own blocks ------------------------------------

static void *own_blocks(void *arg) {
  worker *w = (worker *)arg;
  mars_rng rng;
  mars_rng_seed(&rng, w->seed);
  uint8_t pattern[MAX_DRAW];
  uint8_t back[MAX_DRAW];

  for (unsigned i = 0; i < OPS; i++) {
    size_t n = draw_size(&rng);
    void *p = mm_malloc(n);
    if (p == NULL) {
      note(w, "mm_malloc returned NULL");
      continue;
    }
    fill(pattern, n, w->index + i);
    if (mm_write(p, 0, pattern, n) != (int64_t)n) note(w, "mm_write was short");
    // Written through the allocator, so the payload checksum is established and
    // mm_verify is checking all of the block rather than only its metadata.
    if (mm_verify(p) != MM_OK) note(w, "mm_verify on an intact block");
    if (mm_read(p, 0, back, n) != (int64_t)n) note(w, "mm_read was short");
    if (memcmp(back, pattern, n) != 0) note(w, "payload came back different");

    mm_free(p);
    if (mm_last_error() != MM_OK) note(w, "mm_free reported something");
    w->completed++;
  }
  return NULL;
}

MM_TEST(threads, concurrent_allocation_leaves_a_consistent_heap) {
  REQUIRE_NOT_NULL(g_heap);
  REQUIRE_EQ(mm_init(g_heap, ARENA_SIZE), 0);
  // mm_init deliberately does not clear the counters -- they outlive an arena
  // so that a program can install a second one and still see what the first
  // did -- so a case that asserts on them has to say where it is counting from.
  mm_stats_reset();

  worker w[THREADS];
  run_workers(mm_failures_, w, THREADS, own_blocks);

  uint64_t done = 0;
  for (unsigned i = 0; i < THREADS; i++) done += w[i].completed;
  CHECK_EQ(done, (uint64_t)THREADS * OPS);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // Every block was freed by the thread that allocated it, so the arena should
  // be holding nothing at all -- and the counters, which are the thing threads
  // would quietly corrupt, should add up to what the threads actually did.
  mm_stats_t s;
  mm_stats_get(&s);
#ifdef MM_STATS
  CHECK_EQ(s.live_blocks, 0u);
  CHECK_EQ(s.alloc_calls, done);
  CHECK_EQ(s.free_calls, done);
#else
  (void)s;
#endif
}

// --- A block allocated on one thread and freed on another ------------------
//
// The case that breaks a naive per-thread allocator, which is why it is written
// now, before there are any per-thread arenas to break: it is the test the next
// design has to keep passing.

#define HANDOFF 4096

static void *g_queue[HANDOFF];
// Filled before the consumer is started and read after it is joined. The
// handoff is the join, so there is nothing shared here that needs its own
// synchronisation -- what is being tested is the allocator, not a queue.
static size_t g_queue_len;

static void *free_the_queue(void *arg) {
  worker *w = (worker *)arg;
  for (size_t i = 0; i < g_queue_len; i++) {
    if (!mm_owns(g_queue[i])) {
      note(w, "the arena disclaimed a pointer it had handed out");
      continue;
    }
    mm_free(g_queue[i]);
    if (mm_last_error() != MM_OK) note(w, "a cross-thread free reported");
    w->completed++;
  }
  return NULL;
}

MM_TEST(threads, a_block_may_be_freed_by_a_thread_that_did_not_allocate_it) {
  REQUIRE_NOT_NULL(g_heap);
  REQUIRE_EQ(mm_init(g_heap, ARENA_SIZE), 0);

  mars_rng rng;
  mars_rng_seed(&rng, mars_test_seed());
  uint8_t pattern[MAX_DRAW];

  g_queue_len = 0;
  for (size_t i = 0; i < HANDOFF; i++) {
    size_t n = draw_size(&rng);
    void *p = mm_malloc(n);
    REQUIRE_NOT_NULL(p);
    fill(pattern, n, (unsigned)i);
    memcpy(p, pattern, n);
    g_queue[g_queue_len++] = p;
  }

  worker w;
  memset(&w, 0, sizeof(w));
  w.seed = mars_test_seed();
  REQUIRE_EQ(pthread_create(&w.id, NULL, free_the_queue, &w), 0);
  pthread_join(w.id, NULL);
  CHECK_EQ(w.failures, 0);
  CHECK_EQ(w.completed, (uint64_t)HANDOFF);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // And the space really came back. Allocating the same population again has to
  // succeed, which it cannot if those frees only removed the blocks from
  // somebody's book-keeping.
  mars_rng_seed(&rng, mars_test_seed());
  for (size_t i = 0; i < HANDOFF; i++) {
    void *p = mm_malloc(draw_size(&rng));
    REQUIRE_NOT_NULL(p);
    g_queue[i] = p;
  }
  for (size_t i = 0; i < HANDOFF; i++) mm_free(g_queue[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
}

// --- A thread that exits with its blocks still live ------------------------

#define BEQUEATHED 256

static void *g_bequeathed[BEQUEATHED];
static size_t g_bequeathed_size[BEQUEATHED];

static void *allocate_and_exit(void *arg) {
  worker *w = (worker *)arg;
  mars_rng rng;
  mars_rng_seed(&rng, w->seed);
  uint8_t pattern[MAX_DRAW];

  for (size_t i = 0; i < BEQUEATHED; i++) {
    size_t n = draw_size(&rng);
    void *p = mm_malloc(n);
    g_bequeathed[i] = p;
    g_bequeathed_size[i] = n;
    if (p == NULL) {
      note(w, "mm_malloc returned NULL");
      continue;
    }
    fill(pattern, n, (unsigned)i);
    if (mm_write(p, 0, pattern, n) != (int64_t)n) note(w, "mm_write was short");
  }
  return NULL;  // and the thread ends here, still owing every one of them
}

MM_TEST(threads, blocks_outlive_the_thread_that_allocated_them) {
  REQUIRE_NOT_NULL(g_heap);
  REQUIRE_EQ(mm_init(g_heap, ARENA_SIZE), 0);

  worker w;
  memset(&w, 0, sizeof(w));
  w.seed = mars_test_seed() + 7;
  REQUIRE_EQ(pthread_create(&w.id, NULL, allocate_and_exit, &w), 0);
  pthread_join(w.id, NULL);
  CHECK_EQ(w.failures, 0);

  // The thread is gone. Its memory is not: the contents are what it wrote, the
  // integrity metadata still stands up, and the blocks are still ours.
  uint8_t expect[MAX_DRAW];
  uint8_t got[MAX_DRAW];
  for (size_t i = 0; i < BEQUEATHED; i++) {
    if (g_bequeathed[i] == NULL) continue;
    size_t n = g_bequeathed_size[i];
    CHECK_TRUE(mm_owns(g_bequeathed[i]));
    CHECK_EQ(mm_verify(g_bequeathed[i]), MM_OK);
    fill(expect, n, (unsigned)i);
    CHECK_EQ(mm_read(g_bequeathed[i], 0, got, n), (int64_t)n);
    CHECK_MEM_EQ(got, expect, n);
  }
  CHECK_EQ(mm_check_heap(), MM_OK);

  for (size_t i = 0; i < BEQUEATHED; i++) mm_free(g_bequeathed[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
}

// --- A random mix, seeded per thread so a failure replays ------------------

static void *stress(void *arg) {
  worker *w = (worker *)arg;
  mars_rng rng;
  mars_rng_seed(&rng, w->seed);

  void *live[LIVE];
  memset(live, 0, sizeof(live));

  for (unsigned i = 0; i < OPS; i++) {
    size_t slot = (size_t)mars_rng_below(&rng, LIVE);
    switch (mars_rng_below(&rng, 4)) {
      case 0:
      case 1: {  // allocate, replacing whatever the slot held
        if (live[slot] != NULL) mm_free(live[slot]);
        size_t n = draw_size(&rng);
        live[slot] = mm_malloc(n);
        if (live[slot] == NULL) {
          note(w, "mm_malloc returned NULL");
        } else {
          memset(live[slot], (int)(slot & 0xFFu), n);
        }
        break;
      }
      case 2: {  // resize
        if (live[slot] == NULL) break;
        void *q = mm_realloc(live[slot], draw_size(&rng));
        if (q == NULL) {
          note(w, "mm_realloc returned NULL");
          break;
        }
        live[slot] = q;
        break;
      }
      default: {  // check and release
        if (live[slot] == NULL) break;
        if (!mm_owns(live[slot])) note(w, "the arena disclaimed a live block");
        // The payload was written behind the allocator's back with memset, so
        // no payload checksum was ever established and MM_OK is the whole of
        // what an intact block reports here.
        if (mm_verify(live[slot]) != MM_OK) {
          note(w, "mm_verify on an intact block");
        }
        mm_free(live[slot]);
        live[slot] = NULL;
        break;
      }
    }
    w->completed++;
  }

  for (size_t i = 0; i < LIVE; i++) mm_free(live[i]);
  return NULL;
}

MM_TEST(threads, a_random_mix_of_operations_leaves_the_arena_walkable) {
  REQUIRE_NOT_NULL(g_heap);
  REQUIRE_EQ(mm_init(g_heap, ARENA_SIZE), 0);
  mm_stats_reset();

  worker w[THREADS];
  run_workers(mm_failures_, w, THREADS, stress);

  CHECK_EQ(mm_check_heap(), MM_OK);

#ifdef MM_STATS
  mm_stats_t s;
  mm_stats_get(&s);
  CHECK_EQ(s.live_blocks, 0u);
  CHECK_EQ(s.quarantined_blocks, 0u);
#endif
}

#endif  // MM_LOCK != MM_LOCK_NONE

// --- Fixture ---------------------------------------------------------------
//
// One buffer for the whole file, taken before any test runs. Each case
// re-installs it, which is what keeps the cases independent of each other.
//
// Given back at the end, and that is not tidiness: CI memchecks every test
// binary with --errors-for-leak-kinds=all, so a buffer still reachable at exit
// is an error there.

__attribute__((constructor)) static void make_the_arena(void) {
  g_heap = (uint8_t *)malloc(ARENA_SIZE);
}

__attribute__((destructor)) static void drop_the_arena(void) {
  free(g_heap);
  g_heap = NULL;
}
