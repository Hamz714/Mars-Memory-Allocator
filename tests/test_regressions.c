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

// ---------------------------------------------------------------------------
// The arena promise: no corrupted control word makes the allocator read or
// write outside the arena. Measured at 10,000 trials per cell it held under
// `hardened` and `paranoid` and failed twice in 240,000 under `fast`, from a
// boundary tag written about 115 MB past a 262 KB arena. The two trials that
// found it are replayed by seed as ctest cases; these three pin the mechanism,
// and they run on every platform rather than only where fork() does.

// An arena with poison either side of it, so that a write outside it is caught
// as a changed byte rather than only when it happens to land on a mapping that
// is not there. The 115 MB overrun crashed; a 40-byte one would not have.
#define GUARD_BYTES 4096u
#define GUARD_FILL 0xA5u

static uint8_t *guarded_arena_new(size_t size) {
  uint8_t *raw = (uint8_t *)malloc(size + 2 * GUARD_BYTES);
  if (raw == NULL) return NULL;
  memset(raw, GUARD_FILL, size + 2 * GUARD_BYTES);
  return raw;
}

static bool guards_intact(const uint8_t *raw, size_t size) {
  for (size_t i = 0; i < GUARD_BYTES; i++) {
    if (raw[i] != GUARD_FILL) return false;
    if (raw[GUARD_BYTES + size + i] != GUARD_FILL) return false;
  }
  return true;
}

// mm_publish positions both of its writes from the block's own control word:
// the boundary tag lands at the block's end, and the step to the successor is
// the whole extent. So an extent that runs past the arena must be refused
// rather than written through -- and refused loudly, since leaving the tiling
// broken is not an improvement on writing wildly.
MM_TEST(regression, publish_refuses_an_extent_outside_the_arena) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *hole = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(c);
  mm_free(hole);

  mm_block *b = mm_block_of(hole);
  REQUIRE_NOT_NULL(b);
  mm_bin_remove(b);

  // An extent four times the arena, which is the shape the measured failure
  // arrived in: a control word overwritten by something that was never a size.
  mm_set_block_size(b, (size_t)(g_arena.hi - g_arena.lo) * 4);

  CHECK_FALSE(mm_publish(b));
  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // Reported, not swallowed. Which of the two it reports depends on whether the
  // span could be resynchronised, and both are honest answers.
  CHECK_TRUE(mm_last_error() == MM_ERR_CORRUPT_HEADER ||
             mm_last_error() == MM_ERR_CORRUPT_LINKS);
  // And the refusal left something the allocator can still walk: the span was
  // surrendered rather than left as a hole in the tiling.
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));

  free(raw);
}

// Filing a block into a bin writes two link words into it. The rebuild walks
// the tiling to find blocks to file, and under `fast` a header is only
// bounds-checked -- so once anything has lied about an extent, that walk is
// reading payload bytes as control words and writing links into whatever they
// look like. That is how a two-bit flip destroyed a live block's header.
//
// A free block repeats its extent in its last eight bytes, and the rebuild now
// requires the two to agree before it writes anything. Here the phantom is
// planted inside a live payload with a tag that contradicts it: `fast` refuses
// it on the tag, and the profiles with a checksum refuse it on that, so the
// assertion is the same under all three.
MM_TEST(regression, rebuild_does_not_write_into_a_phantom_free_block) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *hole = mm_malloc(256);
  void *live = mm_malloc(1024);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(live);
  REQUIRE_NOT_NULL(c);
  mm_free(hole);

  mm_block *freed = mm_block_of(hole);
  mm_block *victim = mm_block_of(live);
  REQUIRE_NOT_NULL(freed);
  REQUIRE_NOT_NULL(victim);

  // Where the walk will be sent: a 16-aligned address well inside the live
  // block, so that the link words the rebuild would write land on payload bytes
  // belonging to somebody.
  uint8_t *phantom_at = (uint8_t *)(void *)victim + 8 * MM_ALIGNMENT;

  // Stretch the free block over the start of the live one so that stepping past
  // it lands on the phantom, and make it wholly self-consistent -- tag and
  // checksum included -- so the walk has no reason of its own to stop before it
  // gets there. Without this the test would pass for the wrong reason.
  //
  // This is deliberately more damage than a bit flip would do, and it is
  // unrepresentative in one way that matters for what the test may assert: the
  // tag it writes lands inside the live block's payload. So the assertions
  // below are about what the *rebuild* touched, not the payload surviving.
  size_t stretched = (size_t)(phantom_at - (uint8_t *)(void *)freed);
  REQUIRE_TRUE(stretched >= MM_MIN_BLOCK);
  mm_bin_remove(freed);
  mm_set_block_size(freed, stretched);
  mm_write_free_footer(freed);
  mm_seal(freed);

  // The phantom: reads as a free block of a plausible size, with a boundary tag
  // that does not repeat it and no checksum that would vouch for it.
  mm_block *phantom = (mm_block *)(void *)phantom_at;
  size_t phantom_size = 4 * MM_MIN_BLOCK;
  mm_set_word(phantom, phantom_size, 0, false, false);

  uint8_t before[2 * sizeof(uint64_t)];
  memcpy(before, phantom_at + MM_HDR_SIZE, sizeof(before));

  mm_bins_rebuild();

  // The two link words the rebuild would have written are somebody's payload.
  CHECK_MEM_EQ(phantom_at + MM_HDR_SIZE, before, sizeof(before));
  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));

  // And the phantom is in no bin, so nothing overlapping the live block can be
  // handed out. The rebuild files at the head, and no real block shares its
  // size class here, so the head is where a filed phantom would be -- the whole
  // bin is walked all the same, bounded, since the point of the check is that a
  // damaged list cannot be believed.
  size_t bin = mm_bin_of(phantom_size);
  bool filed = false;
  size_t budget = mm_max_blocks();
  for (mm_block *cur = g_arena.bins[bin]; cur != NULL && budget-- > 0;
       cur = (mm_block *)mm_free_link_get(cur, MM_LINK_NEXT, g_arena.secret)) {
    if (cur == phantom) filed = true;
  }
  CHECK_FALSE(filed);

  free(raw);
}

// Recovery surrenders the span between the damage and the next block boundary,
// and only that span. Finding that boundary is a *search*, so a candidate that
// is merely bounds-plausible is not enough: accepting one that is not a block
// boundary leaves the tiling permanently out of step, and every later walk then
// steps through blocks that are not there. That is what turned a single damaged
// free header into a wild write two frees later.
//
// A free block whose header is destroyed has no mirror to rebuild it from under
// any profile -- free blocks spend those bytes on the boundary tag -- so all
// three surrender it, and all three must surrender exactly it.
MM_TEST(regression, recovery_surrenders_exactly_the_damaged_block) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(256);
  void *hole = mm_malloc(256);
  void *keep = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(keep);
  REQUIRE_NOT_NULL(c);
  mm_free(hole);

  mm_block *damaged = mm_block_of(hole);
  REQUIRE_NOT_NULL(damaged);
  size_t real_span = mm_block_size(damaged);
  REQUIRE_TRUE(real_span >= 2 * MM_MIN_BLOCK);

  // A decoy at the first address the scan will look at, sized so that it ends
  // exactly on the real block after the damage. That is what makes it hard: it
  // is bounds-plausible *and* tiles with a genuine header, which is the whole
  // of what the scan used to require. What it cannot do is repeat its own
  // extent in its last eight bytes -- those bytes are the damaged block's own
  // boundary tag, and they say something else.
  mm_block *decoy =
      (mm_block *)(void *)((uint8_t *)(void *)damaged + MM_MIN_BLOCK);
  mm_set_word(decoy, real_span - MM_MIN_BLOCK, 0, false, false);

  // An extent no bounds check can accept, which is what a control word looks
  // like once something that was never a size has been written over it.
  mm_set_block_size(damaged, (size_t)(g_arena.hi - g_arena.lo) * 4);

  // Freeing the block in front of it makes the walk in mm_publish meet the
  // damage, which is the path the measured failure took.
  mm_free(a);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  CHECK_NE(mm_last_error(), MM_OK);
  // Exactly the damaged block: not the remainder of the arena, and not the
  // MM_MIN_BLOCK the decoy would have cut it down to.
  CHECK_EQ(g_arena.lost_bytes, real_span);
  CHECK_EQ(mm_check_heap(), MM_OK);
  // Still serving requests, which is what makes this quarantine rather than
  // fatal damage.
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(keep);
  mm_free(c);
  free(raw);
}

// --- Driving the re-validation after a bin operation -----------------------
//
// `mm_extent_corroborated` stops the bin rebuild filing something that is not
// a block, but "corroborated" is not "proved": a free block's tag is a second
// copy of its extent, not a checksum over it, so a run of bytes carrying both
// can still be filed. Filing writes two link words into it, and if the block
// is not there those land on somebody's metadata.
//
// So every extent absorb_neighbours reads before a bin operation is
// re-established after it. There are four such extents and so four refusal
// points, and each test below aims a rebuild at one specific control word so
// that exactly one of them fires. Each asserts on what was refused rather than
// on mm_last_error() being non-OK, which is reachable here for far too many
// reasons to be evidence of anything.
//
// Three are reachable under every profile. The backward-path check on the
// block being freed is reachable only under `fast`, for a geometric reason set
// out above that test.

// Aims a bin rebuild at one control word.
//
// `lead` is the free block at the front of the arena that the rebuild's walk
// starts from. Stretching it to end sixteen bytes in front of `target` puts a
// phantom exactly where filing it writes over `target`: bin_push writes a free
// block's two link words at `block + MM_HDR_SIZE` and eight bytes further on,
// and the header size and the link that lands move together, so `target - 16`
// is the address under every profile.
//
// The phantom gets everything a rebuild asks of a free block -- a legible
// extent, a boundary tag repeating it, and a checksum where the profile
// carries one. Its extent is four alignment units rather than the floor, so
// that its own tag lands past the target's link words rather than on them.
static void aim_rebuild_at(mm_block *lead, void *target) {
  uint8_t *at = (uint8_t *)target - 16;
  mm_bin_remove(lead);
  mm_set_block_size(lead, (size_t)(at - (uint8_t *)(void *)lead));
  mm_write_free_footer(lead);
  mm_seal(lead);

  mm_block *phantom = (mm_block *)(void *)at;
  mm_set_word(phantom, 4 * MM_ALIGNMENT, 0, false, false);
  mm_write_free_footer(phantom);
  mm_seal(phantom);
}

// Makes the next mm_bin_remove of `b` take the rebuild path, by putting an
// address in its forward link that no node check will accept.
//
// Call this *after* aim_rebuild_at. That helper unlinks a block itself, and a
// rebuild on the way through would put these links straight back -- which is
// how a first attempt at these tests came to pass without reaching a single
// one of the branches it was written for.
static void break_forward_link(mm_block *b) {
  mm_free_link_set(b, MM_LINK_NEXT, (void *)(uintptr_t)MM_ALIGNMENT,
                   g_arena.secret);
}

// The forward neighbour is the block the rebuild overwrites. Its extent was
// read before it came out of its bin and is about to be added to the extent of
// the block being freed, so a stray word there stretches the merged block over
// whatever follows.
//
// Reachable where the last sixteen bytes of an allocated block are not its
// canary. The phantom has to sit sixteen bytes in front of its target, and its
// target here is the block *after* the one being freed -- so the phantom's own
// header lands in the tail of the block being freed. `fast` carries no canary
// and `paranoid` fills those bytes with the tail mirror instead, but under
// `hardened` the canary always overlaps them: a block is round_up(request + 24,
// 16) bytes and the canary ends at request + 24, so at most fifteen bytes of
// rounding can ever separate the two. Destroying it makes mm_free refuse on the
// canary before any coalescing starts, which is correct behaviour and closes
// the path this test needs.
#if !MM_HAS_CANARY || MM_HAS_MIRROR
MM_TEST(regression, coalesce_refuses_a_neighbour_a_bin_operation_changed) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *lead = mm_malloc(256);
  void *pad = mm_malloc(256);
  void *doomed = mm_malloc(512);
  void *after = mm_malloc(256);
  void *tail = mm_malloc(256);
  REQUIRE_NOT_NULL(lead);
  REQUIRE_NOT_NULL(pad);
  REQUIRE_NOT_NULL(doomed);
  REQUIRE_NOT_NULL(after);
  REQUIRE_NOT_NULL(tail);
  mm_free(lead);
  mm_free(after);

  mm_block *walk = mm_block_of(lead);
  mm_block *b = mm_block_of(doomed);
  mm_block *n = mm_block_of(after);
  REQUIRE_NOT_NULL(walk);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(n);
  size_t own = mm_block_size(b);

  aim_rebuild_at(walk, n);
  break_forward_link(n);

  mm_free(doomed);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // The merge was refused: `b` is free at its own extent rather than at the sum
  // of that extent and a word the rebuild left behind. Without the check it
  // stretches over `n` and out the other side.
  CHECK_FALSE(mm_is_used(b));
  CHECK_EQ(mm_block_size(b), own);
  // And `n` was surrendered rather than left free in the tiling and out of
  // every bin.
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(pad);
  free(raw);
}

#endif  // !MM_HAS_CANARY || MM_HAS_MIRROR

// The block being freed is itself the one the rebuild overwrites, on the
// forward path. Its extent was read at the top of absorb_neighbours and is
// about to have the neighbour's added to it; re-reading it is what stops that
// sum being taken over a stray write.
MM_TEST(regression, coalesce_refuses_when_the_freed_block_changed_forward) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *lead = mm_malloc(256);
  void *pad = mm_malloc(256);
  void *doomed = mm_malloc(512);
  void *after = mm_malloc(256);
  void *tail = mm_malloc(256);
  REQUIRE_NOT_NULL(lead);
  REQUIRE_NOT_NULL(pad);
  REQUIRE_NOT_NULL(doomed);
  REQUIRE_NOT_NULL(after);
  REQUIRE_NOT_NULL(tail);
  mm_free(lead);
  mm_free(after);  // the free neighbour the coalesce will reach for

  mm_block *walk = mm_block_of(lead);
  mm_block *b = mm_block_of(doomed);
  mm_block *n = mm_block_of(after);
  REQUIRE_NOT_NULL(walk);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(n);

  aim_rebuild_at(walk, b);
  break_forward_link(n);

  mm_free(doomed);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // The refusal, stated as the thing that did not happen: `b` was never
  // released, so it is still an allocated block rather than a free one
  // stretching over its neighbour. Without the check it is freed at the sum of
  // an extent and a stray word.
  CHECK_TRUE(mm_is_used(b));
#if MM_HAS_MIRROR
  // Paranoid puts `b`'s header back from its tail mirror instead of giving up
  // on it, which is the whole reason for carrying one.
  CHECK_EQ(g_arena.lost_bytes, (size_t)0);
#else
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
#endif
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(pad);
  free(raw);
}

// The same, on the backward path: the predecessor is the block coming out of a
// bin, and the block being freed is what the rebuild overwrites.
//
// **`fast` only, and the reason is geometric rather than a gap in the test.**
// A bin operation only ever writes at `block + MM_HDR_SIZE` or eight bytes
// past that, so a write landing on a 16-aligned control word has to come from
// a block header sixteen bytes in front of it. Where the header is sixteen
// bytes wide, that header's checksum field occupies the eight bytes
// immediately before the target -- which on this path is the predecessor's
// boundary tag, the one thing mm_prev_free_block has to read to reach the
// backward merge at all. Planting the phantom therefore closes the path it was
// aimed at. Under `fast` the header is eight bytes and the tag survives.
#if !MM_HAS_CRC
MM_TEST(regression, coalesce_refuses_when_the_freed_block_changed_backward) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *lead = mm_malloc(256);
  void *pad = mm_malloc(256);
  void *hole = mm_malloc(512);
  void *doomed = mm_malloc(256);
  void *guard = mm_malloc(256);
  REQUIRE_NOT_NULL(lead);
  REQUIRE_NOT_NULL(pad);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(doomed);
  REQUIRE_NOT_NULL(guard);
  mm_free(lead);
  // `hole` becomes the predecessor; `guard` keeps the forward side shut, so the
  // only merge on offer is the backward one.
  mm_free(hole);

  mm_block *walk = mm_block_of(lead);
  mm_block *b = mm_block_of(doomed);
  mm_block *prev = mm_block_of(hole);
  REQUIRE_NOT_NULL(walk);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(prev);

  aim_rebuild_at(walk, b);
  break_forward_link(prev);

  mm_free(doomed);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // `b` was given up rather than folded into its predecessor. Without the check
  // the merge runs and mm_verify reports the block as perfectly healthy.
  CHECK_EQ(mm_verify(doomed), MM_ERR_QUARANTINED);
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(pad);
  free(raw);
}
#endif  // !MM_HAS_CRC

// The predecessor is the block the rebuild overwrites. Its extent was read
// before it came out of its bin and decides where the merged block starts, so
// a stray word there would move the whole run backwards.
MM_TEST(regression, coalesce_refuses_a_predecessor_a_bin_operation_changed) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *lead = mm_malloc(256);
  void *pad = mm_malloc(256);
  void *hole = mm_malloc(512);
  void *doomed = mm_malloc(256);
  void *guard = mm_malloc(256);
  REQUIRE_NOT_NULL(lead);
  REQUIRE_NOT_NULL(pad);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(doomed);
  REQUIRE_NOT_NULL(guard);
  mm_free(lead);
  mm_free(hole);

  mm_block *walk = mm_block_of(lead);
  mm_block *b = mm_block_of(doomed);
  mm_block *prev = mm_block_of(hole);
  REQUIRE_NOT_NULL(walk);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(prev);
  size_t own = mm_block_size(b);

  aim_rebuild_at(walk, prev);
  break_forward_link(prev);

  mm_free(doomed);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // The merge was refused and `b` released at its own extent instead. Without
  // the check the run is anchored at the predecessor and `b` is never marked
  // free at all, so both halves of this are the refusal being observed.
  CHECK_FALSE(mm_is_used(b));
  CHECK_EQ(mm_block_size(b), own);
  // And the predecessor was surrendered rather than left free in the tiling and
  // out of every bin. A free block carries no tail mirror under any profile, so
  // there is nothing to rebuild it from and all three give it up.
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(pad);
  free(raw);
}

// The same re-validation on the allocation path. mm_malloc reads the chosen
// block's extent, takes it out of its bin, and then hands that extent to
// split_block, which subtracts the request from it -- so a stray word there is
// a subtraction that underflows into a block size larger than the arena.
MM_TEST(regression, allocation_refuses_a_block_a_bin_operation_changed) {
  uint8_t *raw = guarded_arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(raw);
  uint8_t *heap = raw + GUARD_BYTES;
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *lead = mm_malloc(256);
  void *pad = mm_malloc(256);
  void *hole = mm_malloc(512);
  void *keep = mm_malloc(256);
  REQUIRE_NOT_NULL(lead);
  REQUIRE_NOT_NULL(pad);
  REQUIRE_NOT_NULL(hole);
  REQUIRE_NOT_NULL(keep);
  mm_free(lead);
  mm_free(hole);  // the block the next 512-byte request will be given

  mm_block *walk = mm_block_of(lead);
  mm_block *chosen = mm_block_of(hole);
  REQUIRE_NOT_NULL(walk);
  REQUIRE_NOT_NULL(chosen);

  aim_rebuild_at(walk, chosen);
  break_forward_link(chosen);

  void *again = mm_malloc(512);

  CHECK_TRUE(guards_intact(raw, ARENA_SIZE));
  // Nothing was handed out. Without the check the block is carved up at an
  // extent the rebuild left behind, and the caller is given a pointer into it.
  CHECK_NULL(again);
  CHECK_NE(mm_last_error(), MM_OK);
  // And the block was given up rather than left free in the tiling and out of
  // every bin.
  CHECK_GT(g_arena.lost_bytes, (size_t)0);
  // The allocator still serves what it still has.
  CHECK_NOT_NULL(mm_malloc(64));

  mm_free(keep);
  free(raw);
}
