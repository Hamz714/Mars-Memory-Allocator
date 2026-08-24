// The block layout itself: sizes, offsets, the control-word encoding, and the
// per-profile overhead the design doc claims.
//
// Every _Static_assert in mm_layout.h fires at compile time simply because
// this file includes it, so a build of this test under a profile is already
// half the check. What follows is the part a compiler cannot do.

#include "mars_test.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

#define ARENA_SIZE (256u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// --- Geometry --------------------------------------------------------------

MM_TEST(layout, the_profile_has_the_geometry_it_advertises) {
  // The header, the trailer and the floor are what everything else is derived
  // from, so pin them per profile rather than trusting the derivation.
#if defined(MARS_PROFILE_FAST)
  CHECK_STR_EQ(mm_profile(), "fast");
  CHECK_EQ(MM_HDR_SIZE, 8);
  CHECK_EQ(MM_TRAIL, 0);
  CHECK_EQ(MM_MIN_BLOCK, 32);
  CHECK_EQ(MM_BLOCK_OFFSET, 8);
#elif defined(MARS_PROFILE_PARANOID)
  CHECK_STR_EQ(mm_profile(), "paranoid");
  CHECK_EQ(MM_HDR_SIZE, 16);
  CHECK_EQ(MM_TRAIL, 24);
  CHECK_EQ(MM_MIN_BLOCK, 48);
  CHECK_EQ(MM_BLOCK_OFFSET, 0);
#else
  CHECK_STR_EQ(mm_profile(), "hardened");
  CHECK_EQ(MM_HDR_SIZE, 16);
  CHECK_EQ(MM_TRAIL, 8);
  CHECK_EQ(MM_MIN_BLOCK, 48);
  CHECK_EQ(MM_BLOCK_OFFSET, 0);
#endif

  CHECK_EQ(mm_metadata_overhead(), MM_HDR_SIZE + MM_TRAIL);
  CHECK_EQ(mm_alignment(), 16);
  CHECK_EQ(sizeof(mm_block), MM_HDR_SIZE);
}

// The whole point of the slack field is that it is narrow. Seven bits was the
// answer, not six: the paranoid profile's 24-byte trailer plus 15 bytes of
// rounding plus an unsplit 32-byte remainder reaches 71.
MM_TEST(layout, the_slack_field_is_wide_enough_and_no_wider) {
  CHECK_LE(MM_SLACK_MAX, MM_W_SLACK_MASK);
  CHECK_EQ(MM_W_SLACK_BITS, 7);

#if defined(MARS_PROFILE_FAST)
  CHECK_EQ(MM_SLACK_MAX, 39);
#elif defined(MARS_PROFILE_PARANOID)
  CHECK_EQ(MM_SLACK_MAX, 71);
  // The value that rules six bits out. If this ever drops to 63 or below the
  // field could be narrowed, and someone should notice deliberately.
  CHECK_GT(MM_SLACK_MAX, 63);
#else
  CHECK_EQ(MM_SLACK_MAX, 63);
#endif
}

MM_TEST(layout, payloads_are_aligned_whatever_the_header_size_is) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  // An 8-byte header only works because the tiling is shifted 8 bytes into the
  // arena. Check that directly, not just its consequence.
  CHECK_EQ((uintptr_t)(mm_sole_span()->lo + MM_HDR_SIZE) % MM_ALIGNMENT, 0);

  for (size_t n = 1; n <= 512; n++) {
    void *p = mm_malloc(n);
    if (p == NULL) {
      MARS_FAIL_("mm_malloc(%zu) returned NULL", n);
      break;
    }
    if ((uintptr_t)p % alignof(max_align_t) != 0) {
      MARS_FAIL_("mm_malloc(%zu) returned %p, not %zu-byte aligned", n, p,
                 (size_t)alignof(max_align_t));
      mm_free(p);
      break;
    }
    mm_free(p);
  }

  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

// A payload pointer is turned back into a header by subtracting a compile-time
// constant, so everything that makes that subtraction legal has to be checked
// before it happens: an arena, a pointer inside it, and the right alignment.
MM_TEST(layout, a_pointer_is_only_a_block_if_the_geometry_says_so) {
  memset(&g_arena, 0, sizeof(g_arena));
  uint8_t stack_byte = 0;
  CHECK_EQ(mm_verify(&stack_byte), MM_ERR_NOT_INITIALIZED);

  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  // Inside the arena, but not on a payload boundary: the header cannot be
  // where subtracting MM_HDR_SIZE would put it.
  CHECK_EQ(mm_verify((uint8_t *)p + 1), MM_ERR_INVALID_PTR);
  CHECK_EQ(mm_verify((uint8_t *)p + MM_ALIGNMENT / 2), MM_ERR_INVALID_PTR);
  // Below the first payload, and past the end of the tiling.
  CHECK_EQ(mm_verify(mm_sole_span()->lo), MM_ERR_INVALID_PTR);
  CHECK_EQ(mm_verify(mm_sole_span()->hi), MM_ERR_INVALID_PTR);
  // Correctly aligned and inside the arena, but landing mid-payload rather
  // than on a block: only the checksum can tell, and under `fast` there is
  // none, so this is asserted only where it is true.
#if MM_HAS_CRC
  CHECK_NE(mm_verify((uint8_t *)p + MM_ALIGNMENT), MM_OK);
#endif

  // And the real pointer still works after all that.
  CHECK_EQ(mm_verify(p), MM_OK);
  mm_free(p);
  free(heap);
}

#if MM_HAS_CRC
// Two arenas with different secrets must not agree on a checksum, or the
// secret is not doing the job of separating one arena's metadata from
// another's. Pinning it is also how the fault injector replays a trial.
MM_TEST(layout, the_arena_secret_changes_what_a_header_checksums_to) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  uint32_t crc[2];
  const uint64_t secrets[2] = {0x1111111111111111ULL, 0x2222222222222222ULL};
  for (int i = 0; i < 2; i++) {
    mm_pin_secret(secrets[i]);
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
    CHECK_EQ(g_arena.secret, secrets[i]);
    void *p = mm_malloc(128);
    REQUIRE_NOT_NULL(p);
    mm_block *b = mm_block_of(p);
    REQUIRE_NOT_NULL(b);
    crc[i] = b->hdr_crc;
    mm_free(p);
  }
  CHECK_NE(crc[0], crc[1]);

  // Back to drawing one per arena, which is the shipped behaviour.
  mm_pin_secret(0);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  CHECK_NE(g_arena.secret, 0);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}
#endif  // MM_HAS_CRC

// --- requested_size round trip ---------------------------------------------

// Storing only the slack is worth nothing if the exact request cannot be got
// back out. Every size from 1 to 4096, which covers both sides of every
// 16-byte step and both sides of the MM_MIN_BLOCK floor.
MM_TEST(layout, requested_size_round_trips_for_every_size_to_4096) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  size_t worst_slack = 0;
  for (size_t n = 1; n <= 4096; n++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
    void *p = mm_malloc(n);
    if (p == NULL) {
      MARS_FAIL_("mm_malloc(%zu) returned NULL", n);
      break;
    }
    mm_block *b = mm_block_of(p);
    if (b == NULL) {
      MARS_FAIL_("mm_block_of failed for a %zu-byte allocation", n);
      break;
    }
    size_t got = mm_requested_size(b);
    if (got != n) {
      MARS_FAIL_("asked for %zu, recovered %zu (block %zu, slack %zu)", n, got,
                 mm_block_size(b), mm_slack(b));
      break;
    }
    if (mm_slack(b) > worst_slack) worst_slack = mm_slack(b);
  }

  // The bound the static assertion rests on must not merely hold, it must be
  // reachable-looking: a bound nowhere near what actually occurs would mean
  // the derivation had drifted from the code.
  CHECK_LE(worst_slack, MM_SLACK_MAX);
  free(heap);
}

// The other half of the bound: a block that keeps a remainder too small to
// split off carries the most slack any block ever does. Manufacture exactly
// that, at the request size that maximises it.
MM_TEST(layout, requested_size_survives_an_unsplit_remainder) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  size_t worst = 0;
  // Drop a request into a hole that is bigger than it needs by less than the
  // split threshold, so the block keeps the remainder. Sweeping the request
  // and the remainder together reaches whatever combination maximises slack,
  // without this test having to know which one that is per profile.
  for (size_t want = 1; want <= 80; want++) {
    for (size_t extra = MM_ALIGNMENT; extra < MM_MIN_BLOCK;
         extra += MM_ALIGNMENT) {
      REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

      size_t hole_size = mm_size_for(want) + extra;
      // The request whose block is exactly `hole_size`: the block is 16-granular
      // and at least MM_MIN_BLOCK, so subtracting the fixed overhead inverts
      // mm_size_for exactly.
      size_t hole_req = hole_size - MM_HDR_SIZE - MM_TRAIL;

      void *keep_a = mm_malloc(64);
      void *hole = mm_malloc(hole_req);
      void *keep_b = mm_malloc(64);
      REQUIRE_NOT_NULL(keep_a);
      REQUIRE_NOT_NULL(hole);
      REQUIRE_NOT_NULL(keep_b);

      mm_block *hb = mm_block_of(hole);
      REQUIRE_NOT_NULL(hb);
      REQUIRE_EQ(mm_block_size(hb), hole_size);
      mm_free(hole);

      void *got = mm_malloc(want);
      REQUIRE_NOT_NULL(got);
      mm_block *gb = mm_block_of(got);
      REQUIRE_NOT_NULL(gb);

      if (mm_block_size(gb) == hole_size) {
        // The whole hole was taken without splitting: the case being tested.
        if (mm_requested_size(gb) != want) {
          MARS_FAIL_("want %zu in a %zu-byte hole recovered as %zu", want,
                     hole_size, mm_requested_size(gb));
          free(heap);
          return;
        }
        if (mm_slack(gb) > worst) worst = mm_slack(gb);
      }

      CHECK_EQ(mm_verify(got), MM_OK);
      mm_free(got);
      mm_free(keep_a);
      mm_free(keep_b);
      CHECK_EQ(mm_check_heap(), MM_OK);
    }
  }

  // The largest slack actually produced must be the bound the field was sized
  // against -- otherwise the derivation in mm_layout.h is describing a
  // different allocator from the one that got built.
  CHECK_EQ(worst, MM_SLACK_MAX);
  free(heap);
}

// --- Overhead --------------------------------------------------------------

#ifdef MM_STATS
MM_TEST(layout, measured_overhead_matches_the_documented_figure) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_stats_reset();

  enum { N = 200, PAYLOAD = 64 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    p[i] = mm_malloc(PAYLOAD);
    REQUIRE_NOT_NULL(p[i]);
  }

  mm_stats_t s;
  mm_stats_get(&s);
  REQUIRE_EQ(s.peak_blocks, N);
  REQUIRE_EQ(s.peak_payload_bytes, (size_t)N * PAYLOAD);

  size_t meta = (s.peak_block_bytes - s.peak_payload_bytes) / s.peak_blocks;

  // The table in the brief: metadata plus an average 8 bytes of 16-alignment
  // rounding. A 64-byte payload lands on that average exactly.
#if defined(MARS_PROFILE_FAST)
  CHECK_EQ(meta, 16);
#elif defined(MARS_PROFILE_PARANOID)
  CHECK_EQ(meta, 48);
#else
  CHECK_EQ(meta, 32);
#endif

  // And that is a real improvement on the ~128 bytes the previous layout cost,
  // not an artefact of measuring something different.
  CHECK_LT(meta, 64);

  for (int i = 0; i < N; i++) mm_free(p[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}
#endif  // MM_STATS

// --- Boundary tags ---------------------------------------------------------

MM_TEST(layout, prev_in_use_tracks_the_neighbour_on_the_left) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_block *hb = mm_block_of(b);
  mm_block *hc = mm_block_of(c);
  REQUIRE_NOT_NULL(hb);
  REQUIRE_NOT_NULL(hc);

  // Everything is allocated, so every block sees its predecessor in use.
  CHECK_TRUE(mm_prev_in_use(hb));
  CHECK_TRUE(mm_prev_in_use(hc));

  // The first block has nothing before it and must still report "in use", so
  // that nothing tries to step backwards off the front of the arena.
  CHECK_TRUE(mm_prev_in_use((const mm_block *)(const void *)mm_sole_span()->lo));

  mm_free(a);
  CHECK_FALSE(mm_prev_in_use(hb));
  // And b can now find a by its boundary tag alone.
  mm_block *back = mm_prev_free_block(hb);
  CHECK_PTR_EQ(back, (const void *)mm_sole_span()->lo);

  mm_free(b);
  // a and b merged, so c's predecessor is the merged free block.
  CHECK_FALSE(mm_prev_in_use(hc));
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(c);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

MM_TEST(layout, a_free_block_repeats_its_extent_in_its_footer) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(512);
  void *b = mm_malloc(512);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);

  mm_block *ha = mm_block_of(a);
  REQUIRE_NOT_NULL(ha);
  size_t n = mm_block_size(ha);
  mm_free(a);

  uint64_t tag;
  memcpy(&tag, (uint8_t *)(void *)ha + n - sizeof(tag), sizeof(tag));
  CHECK_EQ(tag, n);

  mm_free(b);
  free(heap);
}

// A free block spends payload space on its links and its footer. The smallest
// block the allocator will carve has to hold all of it without overlap, which
// is what MM_MIN_BLOCK is derived from -- exercise the smallest real block
// rather than trusting the arithmetic.
MM_TEST(layout, a_minimum_sized_free_block_holds_its_links_and_footer) {
  size_t arena = mm_min_arena();
  uint8_t *heap = arena_new(arena + MM_ALIGNMENT);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, arena), 0);

  CHECK_EQ(mm_check_heap(), MM_OK);

  void *p = mm_malloc(1);
  REQUIRE_NOT_NULL(p);
  CHECK_EQ(mm_verify(p), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(p);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // And it is reusable, which it would not be if freeing it had scribbled a
  // link over its own footer.
  void *q = mm_malloc(1);
  CHECK_NOT_NULL(q);
  mm_free(q);
  free(heap);
}

// --- Block confusion -------------------------------------------------------

#if MM_HAS_CRC
// The checksum is over the control word, the payload checksum, the block's
// index in the arena and the arena secret. Mixing the index in is what makes a
// header copied from elsewhere -- a perfectly self-consistent one -- fail.
MM_TEST(layout, a_header_copied_from_another_block_is_rejected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *b = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);

  mm_block *ha = mm_block_of(a);
  mm_block *hb = mm_block_of(b);
  REQUIRE_NOT_NULL(ha);
  REQUIRE_NOT_NULL(hb);

  // Identical geometry, identical everything -- except which block it is.
  REQUIRE_EQ(mm_block_size(ha), mm_block_size(hb));
  memcpy(hb, ha, MM_HDR_SIZE);

  CHECK_NE(mm_verify(b), MM_OK);
  CHECK_NE(mm_check_heap(), MM_OK);
  free(heap);
}
#endif  // MM_HAS_CRC

#if MM_HAS_CANARY
MM_TEST(layout, the_canary_differs_between_neighbouring_blocks) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(64);
  void *b = mm_malloc(64);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);

  uint64_t ca, cb;
  memcpy(&ca, (uint8_t *)a + 64, sizeof(ca));
  memcpy(&cb, (uint8_t *)b + 64, sizeof(cb));
  CHECK_NE(ca, cb);

  // Copying one block's canary onto the other must not satisfy it, or the
  // canary would be detecting overruns but not misplacement.
  memcpy((uint8_t *)b + 64, &ca, sizeof(ca));
  CHECK_EQ(mm_verify(b), MM_ERR_CORRUPT_CANARY);

  free(heap);
}
#endif  // MM_HAS_CANARY
