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

// Coalescing relocates a block's footer onto ground that belonged to the
// absorbed block. If that leaves the merged block failing its own integrity
// checks, it gets quarantined -- and the arena silently loses the space for
// good. Freeing everything must therefore restore full capacity.
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

// Coalescing backwards must confirm the previous block actually points at this
// one. Trusting `prev` alone means a damaged link merges two blocks using
// geometry that describes neither of them.
MM_TEST(regression, free_does_not_merge_through_a_broken_prev_link) {
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

  // Point a's forward link at c instead of b, re-sealing so the checksum
  // agrees. Freeing b must notice that a does not point back at it.
  mm_header *ha = (mm_header *)(void *)((uint8_t *)a - MM_PREFIX);
  mm_header *hc = (mm_header *)(void *)((uint8_t *)c - MM_PREFIX);
  ha->next = hc;
  mm_seal(ha);

  mm_free(b);

  // Had the merge gone ahead, a would have swallowed a span computed from the
  // wrong neighbour and c's header would sit inside it.
  CHECK_EQ(mm_verify(c), MM_OK);
  char back[32] = {0};
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  free(heap);
}

// A neighbour that fails validation must be dropped from the list, never
// written through. Its size field is what decides where its footer lands, so
// sealing a corrupted neighbour scatters writes across -- and past -- the
// arena. Run under ASan, where such a write is caught rather than tolerated.
MM_TEST(regression, corrupt_successor_is_dropped_not_written_through) {
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

  // Damage c's header without re-sealing, so it still looks like a block but
  // no longer matches its checksum.
  mm_header *hc = (mm_header *)(void *)((uint8_t *)c - MM_PREFIX);
  hc->size ^= 0x20;

  // Freeing a merges it with b, after which the list would run on into c.
  mm_free(a);

  CHECK_EQ(mm_last_error(), MM_ERR_CORRUPT_LINKS);

  // The allocator must still answer, rather than having corrupted itself.
  void *fresh = mm_malloc(64);
  (void)fresh;

  free(heap);
}

// Space the allocator gives up must be counted. Truncating the block list at a
// damaged neighbour abandons everything beyond it, and an abandonment that is
// not recorded is indistinguishable from a heap that lost nothing -- which is
// how a fault-injection run came to report a heap that had shed most of itself
// as having survived intact.
MM_TEST(regression, abandoned_space_is_accounted_for) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  CHECK_EQ(g_arena.lost_bytes, 0);

  void *a = mm_malloc(256);
  void *b = mm_malloc(256);
  void *c = mm_malloc(256);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_free(b);

  // Damage c so that freeing a, which merges forward into b, then runs into a
  // neighbour it cannot trust.
  mm_header *hc = (mm_header *)(void *)((uint8_t *)c - MM_PREFIX);
  hc->size ^= 0x20;

  mm_free(a);
  CHECK_EQ(mm_last_error(), MM_ERR_CORRUPT_LINKS);

  // Whatever was given up has to show up in the ledger.
  CHECK_GT(g_arena.lost_bytes, 0);
  CHECK_LE(g_arena.lost_bytes, ARENA_SIZE);

  free(heap);
}

// A corrupted link must not send a traversal into an unbounded loop. Writing a
// block's own address into its next pointer is the minimal way to produce one.
//
// This one already holds: overwriting the link invalidates the block's
// checksum, so the traversal quarantines it and stops. It is kept as a guard,
// since the property must survive the free-list rework, where links stop being
// covered by the header checksum.
MM_TEST(regression, traversal_terminates_on_cyclic_link) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *b = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  mm_free(b);

  // Point the first block's forward link at itself, and re-seal it so the
  // checksum agrees. Without re-sealing, the checksum catches the edit and the
  // cycle is never actually walked.
  mm_header *first = (mm_header *)(void *)heap;
  first->next = first;
  mm_seal(first);

  // Any allocation now has to walk the list. It must terminate -- returning
  // NULL is an acceptable outcome, hanging is not.
  void *c = mm_malloc(64);
  (void)c;

  CHECK_EQ(mm_check_heap(), MM_ERR_CORRUPT_LINKS);

  mm_free(a);
  free(heap);
}
