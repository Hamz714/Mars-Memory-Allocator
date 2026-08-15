// Regression tests. Each one pins a specific behaviour that the allocator got
// wrong; they are named for the property they guarantee, not for the defect.
//
// Several of these fail hard (wild read, non-termination) rather than merely
// reporting a mismatch, because that is the nature of the bug being pinned.
// Run a single one with --filter <name> when investigating.

#include "mars_test.h"

#include <stdalign.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

#define ARENA_SIZE (64u * 1024u)

// malloc is guaranteed to return storage aligned for any fundamental type,
// which satisfies what mm_init requires of an arena.
static uint8_t *arena_new(size_t size) {
  return (uint8_t *)malloc(size);
}

// ---------------------------------------------------------------------------

// A write must be able to touch part of a block. Requiring offset-0 writes to
// span the whole payload makes the common case -- writing a short string into
// a larger buffer -- impossible.
MM_TEST(regression, write_partial_at_offset_zero) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  const char msg[] = "Hello, Mars!";
  CHECK_EQ(mm_write(p, 0, msg, sizeof(msg)), (int)sizeof(msg));

  char back[64] = {0};
  CHECK_EQ(mm_read(p, 0, back, sizeof(msg)), (int)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  mm_free(p);
  free(heap);
}

// Reading a freshly allocated block must succeed. It returns unspecified
// contents, but it must not be treated as corruption, and above all it must
// not quarantine a perfectly healthy block.
MM_TEST(regression, read_after_malloc_without_write) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  char back[64];
  CHECK_EQ(mm_read(p, 0, back, sizeof(back)), (int)sizeof(back));

  // If the read quarantined the block, this write fails too.
  const char msg[] = "still alive";
  CHECK_EQ(mm_write(p, 0, msg, sizeof(msg)), (int)sizeof(msg));

  mm_free(p);
  free(heap);
}

// How many 128-byte blocks the arena yields before it is exhausted. Used as a
// capacity probe: any block the allocator quarantines stops being counted.
static int capacity_128(uint8_t *heap, size_t size) {
  if (mm_init(heap, size) != 0) return -1;
  int n = 0;
  while (mm_malloc(128) != NULL) n++;
  return n;
}

// Coalescing relocates a free block's boundary tag onto ground that belonged
// to the absorbed block. If that leaves the merged block failing its own
// integrity checks, it gets quarantined -- and the arena silently loses the
// space for good. Freeing everything must therefore restore full capacity.
MM_TEST(regression, footer_valid_after_coalesce) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  int baseline = capacity_128(heap, ARENA_SIZE);
  REQUIRE_TRUE(baseline > 4);

  // Fresh arena: allocate three neighbours, then free two so that the second
  // free coalesces backward into the first.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_free(b);
  mm_free(a);  // coalesces forward into b, moving the footer
  mm_free(c);

  // Everything is free again, so the arena must hand out just as many blocks
  // as it did when untouched.
  int after = 0;
  while (mm_malloc(128) != NULL) after++;
  CHECK_EQ(after, baseline);

  free(heap);
}

// Every returned pointer must satisfy the platform's maximum fundamental
// alignment, for every request size -- not just for sizes that happen to land
// well. Also run under UBSan: the block headers themselves are cast from
// addresses derived from these same strides.
MM_TEST(regression, headers_aligned_for_odd_sizes) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  static const size_t sizes[] = {1, 7, 17, 50, 63, 100, 127, 200};
  void *held[sizeof(sizes) / sizeof(sizes[0])] = {0};

  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    held[i] = mm_malloc(sizes[i]);
    if (held[i] == NULL) {
      MARS_FAIL_("mm_malloc(%zu) returned NULL", sizes[i]);
      continue;
    }
    uintptr_t addr = (uintptr_t)held[i];
    if (addr % alignof(max_align_t) != 0) {
      MARS_FAIL_("mm_malloc(%zu) returned %p, not %zu-byte aligned", sizes[i],
                 held[i], (size_t)alignof(max_align_t));
    }
  }

  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    if (held[i] != NULL) mm_free(held[i]);
  }
  free(heap);
}

// offset + len must not be allowed to wrap. If it does, the bounds check
// passes and the copy runs against a wild address.
//
// Note this currently passes for the wrong reason: the payload-checksum
// verification rejects the call before the bounds check is ever reached. It
// only becomes a real exercise of the arithmetic once that is fixed, so keep
// it in view while changing the access path.
MM_TEST(regression, bounds_check_resists_offset_overflow) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  char buf[8] = {0};
  CHECK_EQ(mm_read(p, SIZE_MAX - 4, buf, sizeof(buf)), -1);
  CHECK_EQ(mm_write(p, SIZE_MAX - 4, buf, sizeof(buf)), -1);

  // A plain out-of-range offset must be rejected too.
  CHECK_EQ(mm_read(p, 64, buf, 1), -1);
  CHECK_EQ(mm_read(p, 0, buf, 65), -1);

  mm_free(p);
  free(heap);
}

// Pointers that never came from this arena must be rejected without the
// allocator dereferencing anything near them. Under ASan the arena is bounded
// by redzones, so an unchecked read just outside it is caught.
MM_TEST(regression, rejects_pointer_outside_arena) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  // A separate allocation, definitively outside the arena. The pointer handed
  // over is the very START of it, so that recovering a header by stepping
  // backwards from it reads before the allocation -- which ASan traps as a
  // heap-buffer-underflow. Without ASan this test can pass while still
  // performing the out-of-bounds read, so it is meaningful only there.
  uint8_t *foreign = (uint8_t *)malloc(256);
  REQUIRE_NOT_NULL(foreign);
  memset(foreign, 0, 256);

  char buf[8];
  CHECK_EQ(mm_read(foreign, 0, buf, sizeof(buf)), -1);
  CHECK_EQ(mm_write(foreign, 0, buf, sizeof(buf)), -1);
  mm_free(foreign);  // must be ignored, not acted on

  // The real block must be untouched by all that.
  const char msg[] = "arena intact";
  CHECK_EQ(mm_write(p, 0, msg, sizeof(msg)), (int)sizeof(msg));

  free(foreign);
  mm_free(p);
  free(heap);
}

// Growing in place must not underflow when computing the space required.
MM_TEST(regression, realloc_grow_no_size_underflow) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  const char msg[] = "payload survives realloc";
  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int)sizeof(msg));

  // Grow, shrink, and grow again across the split threshold.
  void *q = mm_realloc(p, 4096);
  REQUIRE_NOT_NULL(q);
  char back[64] = {0};
  CHECK_EQ(mm_read(q, 0, back, sizeof(msg)), (int)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  void *r = mm_realloc(q, 32);
  REQUIRE_NOT_NULL(r);

  void *s = mm_realloc(r, 8192);
  REQUIRE_NOT_NULL(s);

  mm_free(s);
  free(heap);
}

// Coalescing backwards must confirm that the boundary tag it stepped over and
// the header it landed on describe the same block. The tag is the only record
// of a free block's extent and it lives in payload space, uncovered by any
// checksum, so trusting it on its own means a damaged tag merges two blocks
// using geometry that describes neither of them.
//
// Under the previous layout this was a stale `prev` pointer; the mechanism
// changed with boundary tags but the hazard is the same one.
MM_TEST(regression, free_does_not_merge_through_a_broken_backward_step) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "c is a bystander";
  REQUIRE_EQ(mm_write(c, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  mm_free(a);  // a is now free, and sits immediately before b

  mm_block *ha = (mm_block *)(void *)g_arena.lo;
  mm_block *hb = mm_block_of(b);
  REQUIRE_NOT_NULL(hb);
  CHECK_PTR_EQ(mm_prev_free_block(hb), ha);

  // Overstate a's extent in its boundary tag, so that stepping backwards from
  // b would land in front of a and swallow ground that is not free.
  uint64_t lie = (uint64_t)mm_block_size(ha) + MM_ALIGNMENT;
  memcpy((uint8_t *)(void *)hb - sizeof(lie), &lie, sizeof(lie));

  // Freeing b must notice that the tag and the header disagree and refuse the
  // backward merge rather than acting on it.
  CHECK_NULL(mm_prev_free_block(hb));
  mm_free(b);

  CHECK_EQ(mm_verify(c), MM_OK);
  char back[32] = {0};
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  free(heap);
}

// A neighbour that fails validation must never be written through before it is
// understood. Its control word is what decides where its own trailer lands, so
// sealing a corrupted neighbour scatters writes across -- and past -- the
// arena. Run under ASan, where such a write is caught rather than tolerated.
#if MM_HAS_CRC
MM_TEST(regression, corrupt_successor_is_never_written_through_blindly) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_free(b);  // leaves a free block directly after a

  // Damage c's header without re-sealing, so it still looks structurally like
  // a block but no longer matches its checksum.
  mm_block *hc = mm_block_of(c);
  REQUIRE_NOT_NULL(hc);
  hc->word ^= (uint64_t)1 << MM_W_SIZE_SHIFT;

  // Freeing a merges it with b, after which the walk runs on into c.
  mm_free(a);

  // Damage is reported either way; what matters is that the allocator noticed
  // and did not scatter writes computed from an extent it could not trust.
  CHECK_NE(mm_last_error(), MM_OK);

  // The allocator must still answer, rather than having corrupted itself, and
  // the arena must still tile.
  void *fresh = mm_malloc(64);
  (void)fresh;
  CHECK_EQ(mm_check_heap(), MM_OK);

  free(heap);
}
#endif  // MM_HAS_CRC

#if MM_HAS_CRC
// A header that no longer checksums leaves the walk unable to step forward:
// the extent it recorded is exactly what was lost. Whatever happens next, it
// must cost at most that one block -- abandoning everything downstream would
// cost the whole arena, and an abandonment nobody counts looks exactly like a
// heap that lost nothing.
//
// This is the clearest place the profiles differ, so both answers are pinned
// rather than one of them being written off as "not applicable".
MM_TEST(regression, damage_met_by_the_walk_costs_at_most_one_block) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *victim = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "c is downstream of the damage";
  const char kept[] = "the victim's own payload";
  REQUIRE_EQ(mm_write(c, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  REQUIRE_EQ(mm_write(victim, 0, kept, sizeof(kept)), (int64_t)sizeof(kept));

  mm_block *hv = mm_block_of(victim);
  REQUIRE_NOT_NULL(hv);
  size_t span = mm_block_size(hv);

  mm_free(b);
  hv->word ^= (uint64_t)1 << MM_W_SIZE_SHIFT;  // its extent is now a lie

  mm_free(a);  // merges forward into b, then meets the damaged header
  CHECK_NE(mm_last_error(), MM_OK);

  char back[64] = {0};
#if MM_HAS_MIRROR
  // The tail mirror sits immediately before the point the scan resynchronised
  // on, so the damaged block is identified exactly and put back. Nothing is
  // surrendered and the payload is still there.
  CHECK_EQ(g_arena.lost_bytes, 0);
  CHECK_EQ(mm_verify(victim), MM_OK);
  CHECK_EQ(mm_read(victim, 0, back, sizeof(kept)), (int64_t)sizeof(kept));
  CHECK_STR_EQ(back, kept);
  memset(back, 0, sizeof(back));
#else
  // Nothing to rebuild from, so the block's span is surrendered -- and exactly
  // that span, not a byte more.
  CHECK_EQ(g_arena.lost_bytes, span);
#endif
  (void)span;

  // Either way the arena still tiles, and c is untouched.
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(mm_verify(c), MM_OK);
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  free(heap);
}
#endif  // MM_HAS_CRC

#if MM_HAS_MIRROR
// Repair reads a mirror it did not put there, so it can be offered one that
// belongs to a different block. A wrong repair is worse than no repair: it
// would hand back a block of the wrong extent and overlap a live neighbour.
//
// The mirror planted here is beyond internal reproach -- correct extent,
// correct flags, correct checksum -- because it is a byte-for-byte copy of a
// real one. What it is not is a mirror of *this* block, and the index folded
// into the checksum is what says so.
MM_TEST(regression, recovery_refuses_a_mirror_from_another_block) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *victim = mm_malloc(256);
  void *donor = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(donor);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "c must survive a bad guess";
  REQUIRE_EQ(mm_write(c, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  mm_block *hv = mm_block_of(victim);
  mm_block *hd = mm_block_of(donor);
  REQUIRE_NOT_NULL(hv);
  REQUIRE_NOT_NULL(hd);
  REQUIRE_EQ(mm_block_size(hv), mm_block_size(hd));
  size_t span = mm_block_size(hv);

  memcpy(mm_block_end(hv) - MM_MIRROR_SIZE, mm_block_end(hd) - MM_MIRROR_SIZE,
         MM_MIRROR_SIZE);
  hv->word ^= (uint64_t)1 << MM_W_SIZE_SHIFT;

  mm_free(b);
  mm_free(a);

  // Believing the donor's mirror would have resealed the victim with someone
  // else's metadata. Refusing it costs the victim's span and nothing else.
  CHECK_EQ(g_arena.lost_bytes, span);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(mm_verify(c), MM_OK);
  char back[64] = {0};
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  free(heap);
}
#endif  // MM_HAS_MIRROR

// A corrupted free-list link must not send a traversal into an unbounded loop,
// and must not be spliced through either. The links live in payload space and
// no checksum covers them, so the list defends itself by checking that every
// node it lands on agrees -- and rebuilds from the tiling when one does not.
//
// The single free list became size-classed bins, so the cycle is now planted
// in the head of a named bin rather than in "the" head. The property being
// pinned is unchanged, and it had to survive that rework precisely because
// binning did not give the links any checksum cover they did not have before.
MM_TEST(regression, traversal_terminates_on_cyclic_link) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *b = mm_malloc(128);
  void *c = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);
  mm_free(b);  // a hole between two live blocks, plus the arena tail

  // Point the head of b's bin at itself. Nothing catches the edit before the
  // walk meets it, which is exactly the situation being guarded against.
  mm_block *hb = mm_block_of(b);
  REQUIRE_NOT_NULL(hb);
  mm_block *head = g_arena.bins[mm_bin_of(mm_block_size(hb))];
  REQUIRE_NOT_NULL(head);
  mm_free_link_set(head, MM_LINK_NEXT, head, g_arena.secret);

  // Any allocation now has to walk the list. It must terminate -- returning
  // NULL is an acceptable outcome, hanging is not.
  void *d = mm_malloc(64);
  (void)d;

  // And having noticed, it must leave the arena in a state that checks out
  // rather than merely surviving the walk.
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(g_arena.lost_bytes, 0);

  mm_free(a);
  mm_free(c);
  free(heap);
}
