// Validated reads and writes: bounds, partial ranges, and round-tripping.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"

#define ARENA_SIZE (64u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

MM_TEST(access, write_then_read_round_trips) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  const char msg[] = "Hello, Mars!";
  CHECK_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  char back[64] = {0};
  CHECK_EQ(mm_read(p, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);
  CHECK_EQ(mm_verify(p), MM_OK);

  mm_free(p);
  free(heap);
}

MM_TEST(access, partial_writes_land_at_the_right_offset) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  uint8_t zeros[64] = {0};
  REQUIRE_EQ(mm_write(p, 0, zeros, sizeof(zeros)), 64);

  const uint8_t patch[4] = {0xDE, 0xAD, 0xBE, 0xEF};
  CHECK_EQ(mm_write(p, 20, patch, sizeof(patch)), 4);

  uint8_t back[64];
  CHECK_EQ(mm_read(p, 0, back, sizeof(back)), 64);
  CHECK_MEM_EQ(back + 20, patch, sizeof(patch));
  for (size_t i = 0; i < 64; i++) {
    if (i >= 20 && i < 24) continue;
    if (back[i] != 0) {
      MARS_FAIL_("byte %zu outside the patched range changed to %u", i,
                 back[i]);
      break;
    }
  }

  // And reading back just the patched window works too.
  uint8_t window[4];
  CHECK_EQ(mm_read(p, 20, window, sizeof(window)), 4);
  CHECK_MEM_EQ(window, patch, sizeof(patch));

  mm_free(p);
  free(heap);
}

MM_TEST(access, out_of_bounds_ranges_are_rejected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  uint8_t buf[128] = {0};

  CHECK_EQ(mm_read(p, 0, buf, 65), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_OOB);
  CHECK_EQ(mm_read(p, 64, buf, 1), -1);
  CHECK_EQ(mm_read(p, 65, buf, 0), -1);
  CHECK_EQ(mm_write(p, 0, buf, 65), -1);
  CHECK_EQ(mm_write(p, 64, buf, 1), -1);

  // Exactly filling the payload is in bounds, as is a zero-length range at the
  // very end.
  CHECK_EQ(mm_write(p, 0, buf, 64), 64);
  CHECK_EQ(mm_read(p, 64, buf, 0), 0);

  mm_free(p);
  free(heap);
}

MM_TEST(access, null_buffers_are_rejected_unless_length_is_zero) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  CHECK_EQ(mm_read(p, 0, NULL, 8), -1);
  CHECK_EQ(mm_write(p, 0, NULL, 8), -1);
  CHECK_EQ(mm_read(p, 0, NULL, 0), 0);

  mm_free(p);
  free(heap);
}

MM_TEST(access, access_to_a_freed_block_is_rejected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *keep = mm_malloc(64);  // stops the freed block merging into the arena
  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(keep);
  REQUIRE_NOT_NULL(p);

  uint8_t buf[8] = {0};
  REQUIRE_EQ(mm_write(p, 0, buf, sizeof(buf)), 8);
  mm_free(p);

  CHECK_EQ(mm_read(p, 0, buf, sizeof(buf)), -1);
  CHECK_EQ(mm_write(p, 0, buf, sizeof(buf)), -1);

  mm_free(keep);
  free(heap);
}

MM_TEST(access, every_byte_of_a_block_is_addressable) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  static const size_t sizes[] = {1, 2, 7, 15, 16, 17, 63, 64, 65, 255, 1024};

  for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
    size_t n = sizes[s];
    void *p = mm_malloc(n);
    if (p == NULL) {
      MARS_FAIL_("mm_malloc(%zu) returned NULL", n);
      continue;
    }

    // Write the block one byte at a time, then read it back whole.
    for (size_t i = 0; i < n; i++) {
      uint8_t v = (uint8_t)(i * 7 + s);
      if (mm_write(p, i, &v, 1) != 1) {
        MARS_FAIL_("size %zu: byte write at offset %zu failed", n, i);
        break;
      }
    }
    uint8_t *back = (uint8_t *)malloc(n);
    if (back != NULL) {
      if (mm_read(p, 0, back, n) != (int64_t)n) {
        MARS_FAIL_("size %zu: full read back failed", n);
      } else {
        for (size_t i = 0; i < n; i++) {
          if (back[i] != (uint8_t)(i * 7 + s)) {
            MARS_FAIL_("size %zu: byte %zu read back as %u", n, i, back[i]);
            break;
          }
        }
      }
      free(back);
    }
    CHECK_EQ(mm_verify(p), MM_OK);
    mm_free(p);
  }

  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}
