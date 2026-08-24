// Validated reads and writes: bounds, partial ranges, and round-tripping.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
// For MM_HAS_CRC and MM_HAS_CANARY, which decide which of the mode's claims
// are even available to be dropped, and for mm_usable_size, which is the libc
// side of the same question.
#include "mm_internal.h"

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

// --- Modes -----------------------------------------------------------------
//
// A mode does not change what a block looks like or what the allocator does
// with one. It changes exactly one thing: whether a payload checksum is being
// maintained, and therefore whether mm_verify is entitled to claim it checked
// the payload. Everything below is about that claim.

MM_TEST(access, the_default_mode_is_managed) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  CHECK_EQ(mm_get_mode(), MM_MODE_MANAGED);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  CHECK_EQ(mm_verify(p), MM_OK);

  free(heap);
}

MM_TEST(access, libc_mode_verifies_what_it_can_and_says_so) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  mm_set_mode(MM_MODE_LIBC);
  CHECK_EQ(mm_get_mode(), MM_MODE_LIBC);

  // The block is intact. What has changed is that the payload was not looked
  // at, and MM_OK would have claimed otherwise.
  CHECK_EQ(mm_verify(p), MM_ERR_DEGRADED);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_set_mode(MM_MODE_MANAGED);
  CHECK_EQ(mm_verify(p), MM_OK);

  free(heap);
}

MM_TEST(access, libc_mode_puts_the_canary_past_what_the_caller_may_write) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  // A managed block's payload is exactly what was asked for, and the canary
  // sits immediately after it.
  uint8_t *m = (uint8_t *)mm_malloc(64);
  REQUIRE_NOT_NULL(m);
  CHECK_EQ(mm_usable_size(m), (size_t)64);
  mm_free(m);

  mm_set_mode(MM_MODE_LIBC);
  uint8_t *p = (uint8_t *)mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  // A libc block's payload is the whole block less its trailer, because a
  // program that asks malloc_usable_size is entitled to write every byte of
  // the answer. Rounding and the minimum block size mean that is usually more
  // than was asked for, and writing into it must not be corruption.
  size_t usable = mm_usable_size(p);
  CHECK_GE(usable, (size_t)64);
  memset(p, 0xa5, usable);
  CHECK_EQ(mm_verify(p), MM_ERR_DEGRADED);
  CHECK_EQ(mm_check_heap(), MM_OK);

#if MM_HAS_CANARY
  // One byte past it is a genuine overrun, and is still caught. Metadata is
  // covered in both modes -- that is the point of having two of them rather
  // than one weak one. "I could not look at the payload" must never outrank
  // "I looked at the canary and it was broken".
  p[usable] ^= 0xffu;
  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_CANARY);
#endif

  mm_set_mode(MM_MODE_MANAGED);
  free(heap);
}

MM_TEST(access, mm_init_restores_the_managed_mode) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_mode(MM_MODE_LIBC);
  CHECK_EQ(mm_get_mode(), MM_MODE_LIBC);

  // A program that installs an arena of its own is asking for the managed API,
  // whatever a shim loaded underneath it had set.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  CHECK_EQ(mm_get_mode(), MM_MODE_MANAGED);

  free(heap);
}

#if MM_HAS_CRC

MM_TEST(access, switching_to_libc_mode_drops_the_checksums_it_stops_keeping) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  uint8_t *p = (uint8_t *)mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  const char msg[] = "established through mm_write";
  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_EQ(mm_verify(p), MM_OK);

  mm_set_mode(MM_MODE_LIBC);

  // The store below is exactly what the mode exists to permit. A checksum left
  // behind from before the switch would report this correct program as
  // corrupt, which is why the switch clears them rather than leaving them to
  // go stale.
  p[0] = 'X';
  CHECK_EQ(mm_verify(p), MM_ERR_DEGRADED);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(mm_scrub(64), MM_OK);

  // And going back does not invent one: the bytes were written behind the
  // allocator's back, so there is nothing it can honestly vouch for until the
  // next mm_write.
  mm_set_mode(MM_MODE_MANAGED);
  CHECK_EQ(mm_verify(p), MM_OK);
  p[1] = 'Y';
  CHECK_EQ(mm_verify(p), MM_OK);

  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  p[2] = 'Z';
  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_PAYLOAD);

  free(heap);
}

MM_TEST(access, libc_mode_establishes_no_checksum_even_through_mm_write) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_mode(MM_MODE_LIBC);

  uint8_t *p = (uint8_t *)mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  const char msg[] = "written through the managed call";
  CHECK_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  // mm_write still copies, and mm_read still reads it back. What it does not
  // do is leave a checksum that the very next raw store would falsify.
  char back[64] = {0};
  CHECK_EQ(mm_read(p, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  p[0] = '!';
  CHECK_EQ(mm_read(p, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_EQ(mm_verify(p), MM_ERR_DEGRADED);

  mm_set_mode(MM_MODE_MANAGED);
  free(heap);
}

#endif  // MM_HAS_CRC
