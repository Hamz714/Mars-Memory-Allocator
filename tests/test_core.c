// Arena setup, allocation, release, coalescing and exhaustion.

#include "mars_test.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

#define ARENA_SIZE (64u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// --- Initialisation --------------------------------------------------------

MM_TEST(core, init_accepts_a_valid_arena) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  CHECK_EQ(mm_init(heap, ARENA_SIZE), 0);
  CHECK_EQ(mm_last_error(), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(core, init_rejects_null_and_undersized_arenas) {
  CHECK_EQ(mm_init(NULL, ARENA_SIZE), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_BAD_ARENA);

  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  CHECK_EQ(mm_init(heap, mm_min_arena() - 1), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_BAD_ARENA);

  // Exactly the minimum must be accepted.
  CHECK_EQ(mm_init(heap, mm_min_arena()), 0);

  free(heap);
}

MM_TEST(core, init_rejects_a_misaligned_arena) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  CHECK_EQ(mm_init(heap + 1, ARENA_SIZE - 1), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_BAD_ARENA);

  free(heap);
}

MM_TEST(core, calls_before_init_report_not_initialized) {
  g_arena.base = NULL;
  g_arena.size = 0;
  g_arena.first = NULL;

  CHECK_NULL(mm_malloc(32));
  CHECK_EQ(mm_last_error(), MM_ERR_NOT_INITIALIZED);
  CHECK_EQ(mm_check_heap(), MM_ERR_NOT_INITIALIZED);
}

MM_TEST(core, every_status_has_a_message) {
  static const mm_status_t all[] = {
      MM_OK,           MM_ERR_NOT_INITIALIZED, MM_ERR_BAD_ARENA,
      MM_ERR_INVALID_PTR, MM_ERR_OOB,          MM_ERR_CORRUPT_HEADER,
      MM_ERR_CORRUPT_CANARY, MM_ERR_CORRUPT_PAYLOAD, MM_ERR_CORRUPT_LINKS,
      MM_ERR_DOUBLE_FREE, MM_ERR_NOMEM,        MM_ERR_QUARANTINED};

  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    const char *msg = mm_strerror(all[i]);
    CHECK_NOT_NULL(msg);
    if (msg == NULL) continue;
    // A status that fell through the switch would come back as the catch-all,
    // which is how a newly added code without a message shows up here.
    if (strcmp(msg, "unknown status") == 0) {
      MARS_FAIL_("status %d has no message of its own", (int)all[i]);
    }
    if (msg[0] == '\0') MARS_FAIL_("status %d has an empty message", (int)all[i]);
  }

  // And an out-of-range value must still return something printable.
  CHECK_NOT_NULL(mm_strerror((mm_status_t)9999));
}

// --- Allocation ------------------------------------------------------------

MM_TEST(core, malloc_returns_distinct_aligned_blocks) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(100);
  void *b = mm_malloc(200);
  void *c = mm_malloc(300);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  CHECK_EQ((uintptr_t)a % mm_alignment(), 0);
  CHECK_EQ((uintptr_t)b % mm_alignment(), 0);
  CHECK_EQ((uintptr_t)c % mm_alignment(), 0);

  CHECK_TRUE(a != b && b != c && a != c);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(a);
  mm_free(b);
  mm_free(c);
  free(heap);
}

MM_TEST(core, malloc_rejects_zero_and_absurd_sizes) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  CHECK_NULL(mm_malloc(0));
  CHECK_NULL(mm_malloc(SIZE_MAX));
  CHECK_NULL(mm_malloc(SIZE_MAX - 64));
  CHECK_NULL(mm_malloc(ARENA_SIZE * 2));
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(core, allocations_do_not_overlap) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  enum { N = 32 };
  void *p[N];
  size_t n[N];

  for (int i = 0; i < N; i++) {
    n[i] = (size_t)(8 + i * 13);
    p[i] = mm_malloc(n[i]);
    REQUIRE_NOT_NULL(p[i]);
    memset(p[i], (int)(i + 1), n[i]);
  }

  // Filling each block with a distinct byte would have shown up as a mismatch
  // here if any two ranges shared storage.
  for (int i = 0; i < N; i++) {
    const uint8_t *q = (const uint8_t *)p[i];
    for (size_t k = 0; k < n[i]; k++) {
      if (q[k] != (uint8_t)(i + 1)) {
        MARS_FAIL_("block %d byte %zu was overwritten: %u != %u", i, k, q[k],
                   (unsigned)(i + 1));
        break;
      }
    }
  }

  for (int i = 0; i < N; i++) mm_free(p[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

// --- Release and coalescing ------------------------------------------------

MM_TEST(core, free_ignores_null) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  mm_free(NULL);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(core, double_free_is_detected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  mm_free(p);
  mm_free(p);
  CHECK_EQ(mm_last_error(), MM_ERR_DOUBLE_FREE);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(core, freeing_everything_restores_full_capacity) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  // Capacity of a pristine arena.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  int baseline = 0;
  while (mm_malloc(128) != NULL) baseline++;
  REQUIRE_TRUE(baseline > 8);

  // Same arena, but churned first: allocate a mixture, free it in an awkward
  // order, and confirm every byte comes back.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  enum { N = 24 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    p[i] = mm_malloc((size_t)(16 + (i % 7) * 64));
    REQUIRE_NOT_NULL(p[i]);
  }
  for (int i = 0; i < N; i += 2) mm_free(p[i]);   // evens
  for (int i = 1; i < N; i += 2) mm_free(p[i]);   // then odds
  CHECK_EQ(mm_check_heap(), MM_OK);

  int after = 0;
  while (mm_malloc(128) != NULL) after++;
  CHECK_EQ(after, baseline);

  free(heap);
}

MM_TEST(core, adjacent_free_blocks_are_merged) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(512);
  void *b = mm_malloc(512);
  void *c = mm_malloc(512);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_free(a);
  mm_free(b);

  // mm_check_heap treats two adjacent free blocks as a missed coalesce, so a
  // clean heap here is itself the assertion that the merge happened.
  CHECK_EQ(mm_check_heap(), MM_OK);

  // And the merged hole must be usable as one piece.
  void *big = mm_malloc(900);
  CHECK_NOT_NULL(big);
  CHECK_PTR_EQ(big, a);

  mm_free(big);
  mm_free(c);
  free(heap);
}

MM_TEST(core, exhaustion_is_reported_and_recoverable) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  enum { CAP = 2048 };
  void *p[CAP];
  int n = 0;
  while (n < CAP) {
    void *q = mm_malloc(256);
    if (q == NULL) break;
    p[n++] = q;
  }
  CHECK_TRUE(n > 0);
  CHECK_EQ(mm_last_error(), MM_ERR_NOMEM);
  CHECK_EQ(mm_check_heap(), MM_OK);

  for (int i = 0; i < n; i++) mm_free(p[i]);

  // Space must be available again once everything is released.
  void *again = mm_malloc(256);
  CHECK_NOT_NULL(again);
  mm_free(again);

  free(heap);
}

MM_TEST(core, best_fit_prefers_the_tightest_hole) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  // Lay out holes of different sizes by allocating neighbours and freeing a
  // few of them, keeping the survivors as separators.
  void *big = mm_malloc(1024);
  void *sep1 = mm_malloc(64);
  void *small = mm_malloc(256);
  void *sep2 = mm_malloc(64);
  REQUIRE_NOT_NULL(big);
  REQUIRE_NOT_NULL(sep1);
  REQUIRE_NOT_NULL(small);
  REQUIRE_NOT_NULL(sep2);

  mm_free(big);    // 1024-byte hole
  mm_free(small);  // 256-byte hole

  // A 200-byte request fits both; best fit must take the tighter one.
  void *pick = mm_malloc(200);
  CHECK_PTR_EQ(pick, small);

  mm_free(pick);
  mm_free(sep1);
  mm_free(sep2);
  free(heap);
}
