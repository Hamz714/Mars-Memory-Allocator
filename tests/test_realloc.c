// Resizing: growth, shrinking, in-place paths, and content preservation.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"

#define ARENA_SIZE (64u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// Fills a block with a size-dependent pattern.
static void fill(void *p, size_t n, uint8_t salt) {
  uint8_t *buf = (uint8_t *)malloc(n);
  if (buf == NULL) return;
  for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(i * 31 + salt);
  mm_write(p, 0, buf, n);
  free(buf);
}

// Checks the first `n` bytes still carry that pattern.
static int pattern_holds(void *p, size_t n, uint8_t salt, int *mm_failures_) {
  uint8_t *buf = (uint8_t *)malloc(n);
  if (buf == NULL) return 1;
  int ok = 1;
  if (mm_read(p, 0, buf, n) != (int64_t)n) {
    MARS_FAIL_("read back of %zu bytes failed", n);
    ok = 0;
  } else {
    for (size_t i = 0; i < n; i++) {
      if (buf[i] != (uint8_t)(i * 31 + salt)) {
        MARS_FAIL_("byte %zu changed: %u != %u", i, buf[i],
                   (uint8_t)(i * 31 + salt));
        ok = 0;
        break;
      }
    }
  }
  free(buf);
  return ok;
}

MM_TEST(realloc, null_pointer_behaves_like_malloc) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_realloc(NULL, 128);
  CHECK_NOT_NULL(p);
  CHECK_EQ((uintptr_t)p % mm_alignment(), 0);

  mm_free(p);
  free(heap);
}

MM_TEST(realloc, zero_size_frees_and_returns_null) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  CHECK_NULL(mm_realloc(p, 0));
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(realloc, growth_preserves_contents) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  fill(p, 64, 0x11);

  void *q = mm_realloc(p, 4096);
  REQUIRE_NOT_NULL(q);
  pattern_holds(q, 64, 0x11, mm_failures_);
  CHECK_EQ(mm_verify(q), MM_OK);

  mm_free(q);
  free(heap);
}

MM_TEST(realloc, shrink_preserves_the_surviving_prefix) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(2048);
  REQUIRE_NOT_NULL(p);
  fill(p, 2048, 0x22);

  void *q = mm_realloc(p, 64);
  REQUIRE_NOT_NULL(q);
  pattern_holds(q, 64, 0x22, mm_failures_);
  CHECK_EQ(mm_verify(q), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(q);
  free(heap);
}

MM_TEST(realloc, shrinking_returns_the_surplus_to_the_arena) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  int baseline = 0;
  while (mm_malloc(128) != NULL) baseline++;
  REQUIRE_TRUE(baseline > 8);

  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  void *p = mm_malloc(8192);
  REQUIRE_NOT_NULL(p);
  void *q = mm_realloc(p, 64);
  REQUIRE_NOT_NULL(q);
  mm_free(q);

  int after = 0;
  while (mm_malloc(128) != NULL) after++;
  CHECK_EQ(after, baseline);

  free(heap);
}

MM_TEST(realloc, repeated_growth_keeps_the_payload_intact) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  // Vector-style growth: 16 bytes, repeatedly multiplied by 1.5. Only the
  // originally written prefix is checked -- bytes beyond it were never filled,
  // and realloc makes no promises about them.
  const size_t filled = 16;
  size_t n = filled;
  void *p = mm_malloc(n);
  REQUIRE_NOT_NULL(p);
  fill(p, filled, 0x33);

  for (int step = 0; step < 12; step++) {
    size_t next = n + n / 2 + 1;
    void *q = mm_realloc(p, next);
    if (q == NULL) break;  // arena exhausted; not a failure
    p = q;
    if (!pattern_holds(p, filled, 0x33, mm_failures_)) break;
    n = next;
  }

  CHECK_EQ(mm_verify(p), MM_OK);
  mm_free(p);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

MM_TEST(realloc, growing_into_a_free_neighbour_stays_in_place) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  void *neighbour = mm_malloc(1024);
  void *guard = mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  REQUIRE_NOT_NULL(neighbour);
  REQUIRE_NOT_NULL(guard);

  fill(p, 128, 0x44);
  mm_free(neighbour);

  // The freed neighbour sits immediately after p, so the block can absorb it
  // instead of moving.
  void *q = mm_realloc(p, 512);
  REQUIRE_NOT_NULL(q);
  CHECK_PTR_EQ(q, p);
  pattern_holds(q, 128, 0x44, mm_failures_);

  mm_free(q);
  mm_free(guard);
  free(heap);
}

MM_TEST(realloc, a_failed_grow_leaves_the_original_untouched) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(256);
  REQUIRE_NOT_NULL(p);
  fill(p, 256, 0x55);

  // Far larger than the arena: the move path cannot find room.
  void *q = mm_realloc(p, ARENA_SIZE * 4);
  CHECK_NULL(q);
  CHECK_EQ(mm_last_error(), MM_ERR_NOMEM);

  // The original must still be intact and usable.
  pattern_holds(p, 256, 0x55, mm_failures_);
  CHECK_EQ(mm_verify(p), MM_OK);

  mm_free(p);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}
