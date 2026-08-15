// Corruption detection: checksums, canaries, boundary tags, and quarantine.
//
// Several of these are gated on the profile, and deliberately so. Detection is
// what the metadata buys, so a profile that carries less metadata detects
// less: `fast` has no checksum and no canary, and asserting that it catches a
// flipped header bit would be asserting something untrue. What every profile
// must do -- not crash, not write outside the arena, keep tiling -- is tested
// unconditionally at the bottom of this file.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

#define ARENA_SIZE (64u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// A tiny reproducible generator, so a failure can be replayed from its seed.
static uint64_t rng_state;
static uint64_t next_rand(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

MM_TEST(integrity, a_healthy_block_verifies) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  const char msg[] = "intact";
  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_EQ(mm_verify(p), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(p);
  free(heap);
}

#if MM_HAS_CRC

MM_TEST(integrity, a_flipped_header_bit_is_detected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  // Every single bit of the control word, one run each. A 32-bit CRC over the
  // header detects all of them -- that guarantee is the reason the mirrored
  // footer could be dropped from allocated blocks, so it is checked
  // exhaustively rather than sampled.
  for (unsigned bit = 0; bit < 64; bit++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
    void *p = mm_malloc(128);
    REQUIRE_NOT_NULL(p);

    mm_block *b = mm_block_of(p);
    REQUIRE_NOT_NULL(b);
    b->word ^= (uint64_t)1 << bit;

    if (mm_verify(p) == MM_OK) {
      MARS_FAIL_("flipping control-word bit %u went unnoticed", bit);
      break;
    }
    b->word ^= (uint64_t)1 << bit;
  }

  free(heap);
}

MM_TEST(integrity, a_flipped_checksum_bit_is_detected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  mm_block *b = mm_block_of(p);
  REQUIRE_NOT_NULL(b);
  b->hdr_crc ^= 0x40u;

  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_HEADER);
  CHECK_NE(mm_check_heap(), MM_OK);

  free(heap);
}

// The payload checksum lives in the header and is covered by the header
// checksum, so damaging it is reported as a bad header rather than as a bad
// payload. That is the useful way round: a healthy payload is not surrendered
// because the number describing it got hit.
MM_TEST(integrity, a_flipped_payload_checksum_reads_as_header_damage) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);
  const char msg[] = "established";
  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  mm_block *b = mm_block_of(p);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NE(b->payload_crc, 0);
  b->payload_crc ^= 0x8000u;

  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_HEADER);

  free(heap);
}

MM_TEST(integrity, payload_corruption_is_detected_on_read) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  uint8_t data[128];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
  REQUIRE_EQ(mm_write(p, 0, data, sizeof(data)), 128);

  // Flip a bit behind the allocator's back.
  ((uint8_t *)p)[40] ^= 0x01;

  uint8_t back[128];
  CHECK_EQ(mm_read(p, 0, back, sizeof(back)), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_CORRUPT_PAYLOAD);

  free(heap);
}

MM_TEST(integrity, quarantine_leaves_the_rest_of_the_heap_usable) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *victim = mm_malloc(128);
  void *c = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "survivor";
  REQUIRE_EQ(mm_write(a, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  REQUIRE_EQ(mm_write(c, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  // Wreck the middle block's header, then touch it so the damage is found.
  mm_block *b = mm_block_of(victim);
  REQUIRE_NOT_NULL(b);
  b->word ^= (uint64_t)1 << MM_W_SIZE_SHIFT;  // its extent is now a lie
  mm_free(victim);
  CHECK_NE(mm_last_error(), MM_OK);

  // Its neighbours must be unharmed and still readable.
  char back[32] = {0};
  CHECK_EQ(mm_read(a, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);
  memset(back, 0, sizeof(back));
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  // And the allocator must keep working, with the arena still tiling.
  void *fresh = mm_malloc(64);
  CHECK_NOT_NULL(fresh);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

// A block given up on stays inside the tiling as permanently-allocated space
// nobody owns, so the arena still tiles exactly and the loss is countable
// rather than merely missing.
MM_TEST(integrity, quarantined_space_is_counted_not_lost) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *victim = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(c);

  mm_block *b = mm_block_of(victim);
  REQUIRE_NOT_NULL(b);
  size_t span = mm_block_size(b);

  // Overrun the payload so the canary catches it: the header is still sound,
  // so the extent surrendered is known exactly.
  memset((uint8_t *)victim + 256, 0xFF, 4);
  mm_free(victim);
  CHECK_EQ(mm_last_error(), MM_ERR_QUARANTINED);

  CHECK_EQ(g_arena.lost_bytes, span);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // Quarantined space is never handed back out, and freeing the pointer again
  // is refused rather than acted on.
  mm_free(victim);
  CHECK_EQ(mm_last_error(), MM_ERR_QUARANTINED);
  CHECK_EQ(g_arena.lost_bytes, span);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(a);
  mm_free(c);
  // Its neighbours must not have merged through it.
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

#else  // !MM_HAS_CRC

// The fast profile's whole trade is an eight-byte header with no checksum in
// it. Pin what that costs rather than leaving a gap in the suite where the
// detection tests would be: a flipped header bit is not detected, and the
// allocator must still be structurally sound afterwards.
MM_TEST(integrity, the_fast_profile_trades_detection_for_space) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  CHECK_EQ(mm_metadata_overhead(), 8);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);
  mm_block *b = mm_block_of(p);
  REQUIRE_NOT_NULL(b);

  // A flip in the slack field goes unnoticed -- there is nothing to notice it
  // with. The block is still structurally a block, which is all this profile
  // promises.
  b->word ^= (uint64_t)1 << MM_W_SLACK_SHIFT;
  CHECK_EQ(mm_verify(p), MM_OK);
  CHECK_TRUE(mm_is_block(b));

  free(heap);
}

#endif  // MM_HAS_CRC

#if MM_HAS_CANARY
MM_TEST(integrity, writing_past_the_payload_trips_the_canary) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  // Deliberately overrun by writing straight through the returned pointer,
  // which is exactly what the canary exists to catch.
  memset((uint8_t *)p + 64, 0xFF, 4);

  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_CANARY);

  free(heap);
}
#endif  // MM_HAS_CANARY

#if MM_HAS_MIRROR
// What the paranoid profile buys: a header whose checksum no longer holds is
// put back from the tail mirror instead of being surrendered. The mirror sits
// at a fixed offset from the block's *end*, so once the walk has found where
// the block ends the repair is one bounded check.
MM_TEST(integrity, a_damaged_header_is_rebuilt_from_its_mirror) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *victim = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "the payload outlives its header";
  REQUIRE_EQ(mm_write(victim, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  mm_block *b = mm_block_of(victim);
  REQUIRE_NOT_NULL(b);
  b->hdr_crc ^= 0x11u;  // the mirror is untouched

  CHECK_EQ(mm_verify(victim), MM_ERR_CORRUPT_HEADER);

  // Touching it again repairs rather than surrenders: nothing is lost, and the
  // contents are still there.
  char back[64] = {0};
  CHECK_EQ(mm_read(victim, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);
  CHECK_EQ(g_arena.lost_bytes, 0);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(a);
  mm_free(victim);
  mm_free(c);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

// And when both the header and its mirror are gone there is nothing to rebuild
// from. Only that block's span is surrendered; everything downstream survives.
MM_TEST(integrity, an_unrecoverable_block_costs_only_itself) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *victim = mm_malloc(256);
  void *d = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(d);

  const char msg[] = "d is past the wreckage";
  REQUIRE_EQ(mm_write(d, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  mm_block *b = mm_block_of(victim);
  REQUIRE_NOT_NULL(b);
  size_t span = mm_block_size(b);
  memset(mm_block_end(b) - MM_MIRROR_SIZE, 0, MM_MIRROR_SIZE);
  b->word ^= (uint64_t)1 << MM_W_SIZE_SHIFT;

  mm_free(victim);
  CHECK_NE(mm_last_error(), MM_OK);

  CHECK_EQ(g_arena.lost_bytes, span);
  CHECK_EQ(mm_check_heap(), MM_OK);

  char back[64] = {0};
  CHECK_EQ(mm_verify(d), MM_OK);
  CHECK_EQ(mm_read(d, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  free(heap);
}
#endif  // MM_HAS_MIRROR

// --- Every profile ---------------------------------------------------------

// The boundary tag is the only record of a free block's extent, so a damaged
// one has to be caught by the heap check whatever the profile: without it, a
// backward step would land in the middle of some other block.
MM_TEST(integrity, a_damaged_boundary_tag_is_reported) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);

  mm_block *ha = mm_block_of(a);
  REQUIRE_NOT_NULL(ha);
  size_t n = mm_block_size(ha);
  mm_free(a);

  uint64_t bad = n ^ 0x10;
  memcpy((uint8_t *)(void *)ha + n - sizeof(bad), &bad, sizeof(bad));

  CHECK_EQ(mm_check_heap(), MM_ERR_CORRUPT_LINKS);

  // And stepping backwards over it must refuse rather than land somewhere
  // arbitrary: the tag and the header it claims to describe disagree.
  mm_block *hb = mm_block_of(b);
  REQUIRE_NOT_NULL(hb);
  CHECK_NULL(mm_prev_free_block(hb));

  free(heap);
}

MM_TEST(integrity, a_scrambled_free_list_is_rebuilt_from_the_tiling) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p[6];
  for (int i = 0; i < 6; i++) {
    p[i] = mm_malloc(256);
    REQUIRE_NOT_NULL(p[i]);
  }
  // Free alternate blocks so several holes exist, separated by live ones.
  for (int i = 0; i < 6; i += 2) mm_free(p[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // Scribble over the head's forward link. It is not covered by any checksum
  // -- it lives in payload space -- so the list has to defend itself by
  // checking that the block it lands on agrees.
  mm_block *head = g_arena.free_head;
  REQUIRE_NOT_NULL(head);
  uint64_t junk = 0x1234567812345678ULL;
  memcpy(mm_payload_of(head), &junk, sizeof(junk));

  // Any allocation now has to walk the list. It must notice, rebuild, and
  // still answer -- and the tiling must be untouched by all that.
  void *fresh = mm_malloc(200);
  CHECK_NOT_NULL(fresh);
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(integrity, scattered_bit_flips_never_crash_the_allocator) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  rng_state = mars_test_seed() ? mars_test_seed() : 0x9E3779B97F4A7C15ULL;

  for (int round = 0; round < 40; round++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

    enum { N = 16 };
    void *p[N];
    int live = 0;
    for (int i = 0; i < N; i++) {
      p[i] = mm_malloc(64 + (size_t)(i * 16));
      if (p[i] != NULL) {
        uint8_t v = (uint8_t)i;
        mm_write(p[i], 0, &v, 1);
        live++;
      }
    }
    REQUIRE_TRUE(live > 0);

    // Spray bit flips across the whole arena.
    for (int f = 0; f < 24; f++) {
      size_t pos = (size_t)(next_rand() % ARENA_SIZE);
      heap[pos] ^= (uint8_t)(1u << (next_rand() % 8));
    }

    // Every one of these must either succeed or report an error. What must not
    // happen is a crash, a hang, or a write outside the arena -- which is why
    // this test is run under ASan and Valgrind as well as on its own.
    for (int i = 0; i < N; i++) {
      if (p[i] == NULL) continue;
      uint8_t back[256];
      (void)mm_verify(p[i]);
      (void)mm_read(p[i], 0, back, 64);
      (void)mm_write(p[i], 0, back, 8);
    }
    (void)mm_check_heap();
    for (int i = 0; i < N; i++) {
      if (p[i] != NULL) mm_free(p[i]);
    }
    (void)mm_malloc(128);
  }

  free(heap);
}
