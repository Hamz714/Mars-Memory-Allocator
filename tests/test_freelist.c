// Size-classed bins, the bitmap over them, and the patrol that covers the
// memory binning stops touching.
//
// The bin index is the one piece of arithmetic the whole search rests on --
// every "this bin's head fits without looking at it" shortcut is a consequence
// of it being monotonic -- so it is checked exhaustively rather than sampled.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mars_rng.h"
#include "mm_internal.h"

#define ARENA_SIZE (256u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// The bin a live or freed pointer's block belongs to.
static size_t bin_of_ptr(const void *ptr) {
  mm_block *b = mm_block_of(ptr);
  return b == NULL ? SIZE_MAX : mm_bin_of(mm_block_size(b));
}

// Whether the bitmap and the bin heads tell the same story. mm_check_heap
// checks this too, but a test that can point at the offending bin is worth
// more than one that reports "the arena is inconsistent".
static int bitmap_disagrees(void) {
  for (size_t i = 0; i < MM_BIN_COUNT; i++) {
    bool marked = (g_arena.bin_bitmap[i / 64] >> (i % 64)) & 1u;
    if (marked != (g_arena.bins[i] != NULL)) return (int)i;
  }
  return -1;
}

// ---------------------------------------------------------------------------
// The index

// Monotonic non-decreasing over every block size the arena can hold. This is
// what lets the search take the head of any bin above the requested one
// without examining it: if bin(size) > bin(need) then size > need, which is
// exactly the contrapositive of this property.
MM_TEST(freelist, bin_index_is_monotonic_over_every_block_size) {
  size_t previous = mm_bin_of(MM_MIN_BLOCK);
  CHECK_EQ(previous, 0);

  // Every 16-granular size to a megabyte, then a coarser stride to well past
  // the largest bin so the clamp on the top one is covered too. The fine part
  // is where the two schemes meet and where the octave boundaries fall close
  // together; beyond it a bin is wider than the stride, so no bin is skipped.
  const size_t fine = (size_t)1 << 20;
  const size_t limit = (size_t)1 << 28;
  for (size_t n = MM_MIN_BLOCK; n <= limit;
       n += (n < fine ? MM_ALIGNMENT : 256 * MM_ALIGNMENT)) {
    size_t bin = mm_bin_of(n);
    if (bin < previous) {
      MARS_FAIL_("bin_of(%zu) = %zu went backwards from %zu", n, bin, previous);
      return;
    }
    if (bin >= MM_BIN_COUNT) {
      MARS_FAIL_("bin_of(%zu) = %zu is past the last bin", n, bin);
      return;
    }
    previous = bin;
  }
  // The top bin really is reached, rather than the range simply running out.
  CHECK_EQ(mm_bin_of(limit), MM_BIN_COUNT - 1);
  CHECK_EQ(mm_bin_of(SIZE_MAX / 2), MM_BIN_COUNT - 1);
}

// Below MM_LARGE_MIN a bin is an exact class: one size, and nothing else in
// it. That is what makes a small allocation a pop rather than a search.
MM_TEST(freelist, small_bins_are_exact_classes) {
  for (size_t i = 0; i < MM_SMALL_BINS; i++) {
    size_t n = MM_MIN_BLOCK + i * MM_ALIGNMENT;
    CHECK_EQ(mm_bin_of(n), i);
    // The next size up and the one down must be somewhere else entirely.
    if (i + 1 < MM_SMALL_BINS) CHECK_EQ(mm_bin_of(n + MM_ALIGNMENT), i + 1);
    if (i > 0) CHECK_EQ(mm_bin_of(n - MM_ALIGNMENT), i - 1);
  }
  // And the join between the two schemes is a step of exactly one bin.
  CHECK_EQ(mm_bin_of(MM_LARGE_MIN - MM_ALIGNMENT), MM_SMALL_BINS - 1);
  CHECK_EQ(mm_bin_of(MM_LARGE_MIN), MM_SMALL_BINS);
}

// Every size lands in a bin whose class it actually fits. Stated as the search
// uses it: if a block's bin is above a request's bin, the block is big enough
// for that request -- which is why taking the head of any higher bin needs no
// size comparison at all. It is the contrapositive of monotonicity, checked
// here directly over every pair of block sizes a caller can produce.
MM_TEST(freelist, a_higher_bin_only_ever_holds_a_larger_block) {
  static const size_t requests[] = {
      1,    2,    15,     16,     17,     48,       64,        100,
      255,  256,  511,    512,    1000,   1024,     1025,      2000,
      4095, 4096, 16384,  65535,  65536,  1u << 20, 1u << 22,  (1u << 24) + 7};
  enum { N = sizeof(requests) / sizeof(requests[0]) };

  size_t size[N];
  for (int i = 0; i < N; i++) {
    size[i] = mm_size_for(requests[i]);
    REQUIRE_NE(size[i], 0);
    // A request always fits in the block chosen for it, whichever bin that is.
    CHECK_GE(size[i], requests[i] + mm_metadata_overhead());
    CHECK_GE(size[i], MM_MIN_BLOCK);
    CHECK_EQ(size[i] % MM_ALIGNMENT, 0);
  }

  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      if (mm_bin_of(size[i]) > mm_bin_of(size[j]) && size[i] <= size[j]) {
        MARS_FAIL_("bin_of(%zu)=%zu is above bin_of(%zu)=%zu without being "
                   "larger", size[i], mm_bin_of(size[i]), size[j],
                   mm_bin_of(size[j]));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// The bitmap

MM_TEST(freelist, bitmap_tracks_bin_emptiness_through_a_random_sequence) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  mars_rng rng;
  mars_rng_seed(&rng, mars_test_seed() ? mars_test_seed() : 0xC0FFEEULL);

  // The patrol would quarantine nothing here, but leaving it on would mix two
  // effects into one test. This one is about the bitmap.
  mm_set_scrub_interval(0, 0);

  enum { SLOTS = 64 };
  void *live[SLOTS] = {0};

  for (int op = 0; op < 4000; op++) {
    size_t k = (size_t)mars_rng_below(&rng, SLOTS);
    if (live[k] != NULL) {
      mm_free(live[k]);
      live[k] = NULL;
    } else {
      // A spread that reaches both schemes: mostly small, sometimes past
      // MM_LARGE_MIN so the log-spaced bins are exercised as well.
      size_t n = mars_rng_below(&rng, 8) == 0
                     ? (size_t)mars_rng_below(&rng, 8192) + 1
                     : (size_t)mars_rng_below(&rng, 512) + 1;
      live[k] = mm_malloc(n);
    }

    int bad = bitmap_disagrees();
    if (bad >= 0) {
      MARS_FAIL_("after op %d, bin %d disagrees with its bitmap bit", op, bad);
      break;
    }
  }

  CHECK_EQ(mm_check_heap(), MM_OK);
  for (int i = 0; i < SLOTS; i++) mm_free(live[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(bitmap_disagrees(), -1);

  mm_set_scrub_interval(1024, 16);
  free(heap);
}

// ---------------------------------------------------------------------------
// Reuse

// An exact class in LIFO order means the block just freed is the block handed
// back, which is the cache behaviour the small bins exist for. The neighbours
// are held live so that nothing coalesces and the block stays the size it was.
MM_TEST(freelist, a_block_comes_back_from_the_bin_it_went_into) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *before = mm_malloc(100);
  void *target = mm_malloc(100);
  void *after = mm_malloc(100);
  REQUIRE_NOT_NULL(before);
  REQUIRE_NOT_NULL(target);
  REQUIRE_NOT_NULL(after);

  size_t bin = bin_of_ptr(target);
  mm_free(target);
  CHECK_EQ(bin_of_ptr(target), bin);       // freeing does not move its class
  CHECK_PTR_EQ(g_arena.bins[bin], mm_block_of(target));

  void *again = mm_malloc(100);
  CHECK_PTR_EQ(again, target);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(before);
  mm_free(again);
  mm_free(after);
  free(heap);
}

// ---------------------------------------------------------------------------
// Coalescing across bins

// Merging changes a block's size and therefore its bin. Both inputs have to
// leave their bins before they merge and the result has to arrive in the bin
// its new size asks for -- get either half wrong and a bin ends up holding a
// block that is not there any more, or memory is lost from every bin at once.
MM_TEST(freelist, coalescing_moves_the_result_into_its_new_bin) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *guard_lo = mm_malloc(64);
  void *a = mm_malloc(200);
  void *b = mm_malloc(200);
  void *guard_hi = mm_malloc(64);
  REQUIRE_NOT_NULL(guard_lo);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(guard_hi);

  mm_block *ha = mm_block_of(a);
  mm_block *hb = mm_block_of(b);
  REQUIRE_NOT_NULL(ha);
  REQUIRE_NOT_NULL(hb);
  size_t merged_size = mm_block_size(ha) + mm_block_size(hb);
  size_t small_bin = mm_bin_of(mm_block_size(ha));
  size_t merged_bin = mm_bin_of(merged_size);
  REQUIRE_NE(small_bin, merged_bin);

  mm_free(a);
  CHECK_PTR_EQ(g_arena.bins[small_bin], ha);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // Freeing b merges it backwards into a. Both must leave the small bin and
  // the survivor must appear in the bin for the combined extent.
  mm_free(b);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_NULL(g_arena.bins[small_bin]);
  CHECK_PTR_EQ(g_arena.bins[merged_bin], ha);
  CHECK_EQ(mm_block_size(ha), merged_size);
  CHECK_EQ(bitmap_disagrees(), -1);

  // And the merged block is reachable as one block: a request that only the
  // combined extent can satisfy must be met by it.
  void *big = mm_malloc(merged_size - mm_metadata_overhead() - MM_ALIGNMENT);
  CHECK_PTR_EQ(big, mm_payload_of(ha));

  mm_free(big);
  mm_free(guard_lo);
  mm_free(guard_hi);
  free(heap);
}

// A split hands the remainder back, and the remainder belongs in the bin its
// own size asks for, not the one the block came out of.
MM_TEST(freelist, splitting_files_the_remainder_by_its_own_size) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *hold = mm_malloc(4096);
  // Held live so that freeing `hold` cannot merge it into the arena tail; the
  // point here is the remainder of a split, not what coalescing does with it.
  void *stopper = mm_malloc(64);
  REQUIRE_NOT_NULL(hold);
  REQUIRE_NOT_NULL(stopper);
  mm_block *hh = mm_block_of(hold);
  REQUIRE_NOT_NULL(hh);
  size_t whole = mm_block_size(hh);

  mm_free(hold);
  CHECK_EQ(mm_block_size(hh), whole);
  CHECK_PTR_EQ(g_arena.bins[mm_bin_of(whole)], hh);

  // Take a small bite out of it. The block splits, and the two pieces belong
  // in two different bins -- neither of them the one the whole block was in.
  void *bite = mm_malloc(64);
  REQUIRE_NOT_NULL(bite);
  CHECK_PTR_EQ(bite, mm_payload_of(hh));
  size_t taken = mm_block_size(hh);
  mm_block *rest = mm_next_block(hh);

  CHECK_EQ(mm_block_size(rest), whole - taken);
  CHECK_PTR_EQ(g_arena.bins[mm_bin_of(whole - taken)], rest);
  CHECK_NULL(g_arena.bins[mm_bin_of(whole)]);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(bitmap_disagrees(), -1);

  mm_free(bite);
  mm_free(stopper);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

// ---------------------------------------------------------------------------
// Hardened unlink

// A link that points somewhere it should not must be rejected before it is
// dereferenced, and the bins rebuilt from the tiling. Run under ASan, where a
// splice through the damaged node would be caught as a write outside the
// arena rather than merely producing a wrong answer.
MM_TEST(freelist, a_damaged_link_is_rejected_rather_than_spliced_through) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  // Somewhere definitively outside the arena, so that a splice through it
  // would be a write to another allocation entirely -- which is what ASan is
  // here to catch.
  uint8_t *foreign = (uint8_t *)malloc(64);
  REQUIRE_NOT_NULL(foreign);
  memset(foreign, 0, 64);

  // Four ways a link can be wrong, one per check the unlink makes: outside the
  // arena, misaligned inside it, pointing at a block that is in use, and
  // pointing at a genuinely free block that belongs to another bin. All four
  // have to be refused before anything is written.
  for (int variant = 0; variant < 4; variant++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

    void *keep[6];
    for (int i = 0; i < 6; i++) {
      keep[i] = mm_malloc(128);
      REQUIRE_NOT_NULL(keep[i]);
    }
    void *live = mm_malloc(4096);
    REQUIRE_NOT_NULL(live);

    // Two holes of the same class, so the bin has a head with a successor.
    mm_free(keep[1]);
    mm_free(keep[3]);
    REQUIRE_EQ(mm_check_heap(), MM_OK);

    size_t bin = bin_of_ptr(keep[1]);
    mm_block *head = g_arena.bins[bin];
    REQUIRE_NOT_NULL(head);

    void *bogus = NULL;
    switch (variant) {
      case 0: bogus = foreign; break;                   // not in the arena
      case 1: bogus = (void *)(g_arena.lo + 8); break;   // misaligned
      case 2: bogus = mm_block_of(keep[0]); break;       // an allocated block
      default: {
        // A block that is genuinely free and passes every structural check --
        // it is simply in another class. Only the bin test catches this one.
        size_t j = MM_BIN_COUNT;
        while (j-- > 0) {
          if (j != bin && g_arena.bins[j] != NULL) break;
        }
        bogus = j < MM_BIN_COUNT ? (void *)g_arena.bins[j] : NULL;
        break;
      }
    }
    REQUIRE_NOT_NULL(bogus);
    mm_free_link_set(head, MM_LINK_NEXT, bogus, g_arena.secret);

    // Any allocation that reaches this bin has to walk it. It must notice, and
    // it must still answer.
    void *fresh = mm_malloc(128);
    CHECK_NOT_NULL(fresh);
    CHECK_NE(mm_last_error(), MM_OK);

    // The tiling was never the thing that was damaged, so nothing may have
    // been surrendered, and the bins must agree with it again.
    CHECK_EQ(mm_check_heap(), MM_OK);
    CHECK_EQ(g_arena.lost_bytes, 0);
    CHECK_EQ(bitmap_disagrees(), -1);

    // The bystanders are untouched.
    CHECK_EQ(mm_verify(keep[0]), MM_OK);
    CHECK_EQ(mm_verify(live), MM_OK);
  }

  free(foreign);
  free(heap);
}

// The head pointer and the bitmap live outside the arena, in the allocator's
// own state, so no checksum can cover them and nothing stops a stray write
// through a stale `mm_arena *`. Every operation that consults them therefore
// has to survive finding them wrong: allocating from a bin, freeing into one,
// and falling through to a bin the bitmap only claims is occupied.
MM_TEST(freelist, a_clobbered_bin_head_is_survived_by_every_operation) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  for (int variant = 0; variant < 3; variant++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

    // Eight blocks with holes punched at 2 and 5, so that both holes have live
    // neighbours and neither the frees nor the allocations below coalesce into
    // a different size class.
    void *keep[8];
    for (int i = 0; i < 8; i++) {
      keep[i] = mm_malloc(128);
      REQUIRE_NOT_NULL(keep[i]);
    }
    mm_free(keep[2]);
    REQUIRE_EQ(mm_check_heap(), MM_OK);

    size_t bin = bin_of_ptr(keep[2]);
    REQUIRE_TRUE(bin < MM_BIN_COUNT);

    switch (variant) {
      case 0:
        // The head now names a block that is allocated. Freeing into this bin
        // has to refuse to chain through it.
        g_arena.bins[bin] = mm_block_of(keep[0]);
        mm_free(keep[5]);
        break;
      case 1:
        // The same, met from the allocation side: the scan of the bin the
        // request maps to lands on a head that does not stand up.
        g_arena.bins[bin] = mm_block_of(keep[0]);
        CHECK_NOT_NULL(mm_malloc(128));
        break;
      default: {
        // The bitmap claims a bin that is empty. A request whose own class is
        // empty falls through to the next non-empty bin the bitmap names, and
        // must not follow the null head it finds there.
        size_t want = mm_bin_of(mm_size_for(200));
        REQUIRE_TRUE(want + 1 < MM_BIN_COUNT);
        REQUIRE_TRUE(g_arena.bins[want] == NULL);
        REQUIRE_TRUE(g_arena.bins[want + 1] == NULL);
        g_arena.bin_bitmap[(want + 1) / 64] |= (uint64_t)1 << ((want + 1) % 64);
        CHECK_NOT_NULL(mm_malloc(200));
        break;
      }
    }

    CHECK_NE(mm_last_error(), MM_OK);
    // Rebuilt from the tiling, which was never what was damaged: nothing
    // surrendered, everything agreeing again.
    CHECK_EQ(mm_check_heap(), MM_OK);
    CHECK_EQ(g_arena.lost_bytes, 0);
    CHECK_EQ(bitmap_disagrees(), -1);
    CHECK_EQ(mm_verify(keep[0]), MM_OK);
  }

  free(heap);
}

// A block whose predecessor link is null although it is not the head of its
// bin. Only the head may have no predecessor, and a node claiming otherwise
// must not be spliced -- the splice would write through g_arena.bins[bin] and
// lose every block behind the real head.
MM_TEST(freelist, a_node_that_is_not_the_head_may_not_claim_to_be) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *b = mm_malloc(128);
  void *c = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  mm_free(b);
  size_t bin = bin_of_ptr(b);
  REQUIRE_TRUE(bin < MM_BIN_COUNT);
  CHECK_PTR_EQ(g_arena.bins[bin], mm_block_of(b));

  // Lose the head pointer. b still says it has no predecessor, which is now a
  // claim the bin contradicts.
  g_arena.bins[bin] = NULL;

  // Freeing a merges forward into b, so b has to come out of its bin first.
  mm_free(a);
  CHECK_NE(mm_last_error(), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_EQ(g_arena.lost_bytes, 0);
  CHECK_EQ(bitmap_disagrees(), -1);

  mm_free(c);
  CHECK_EQ(mm_check_heap(), MM_OK);
  free(heap);
}

// Taking a block out of a bin it was never in must do nothing at all. In
// particular it must not clear the links, because for an allocated block those
// eight-byte slots are the caller's first sixteen payload bytes.
MM_TEST(freelist, unlinking_a_block_that_is_on_no_list_touches_nothing) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  uint8_t written[32];
  for (size_t i = 0; i < sizeof(written); i++) written[i] = (uint8_t)(i + 1);
  REQUIRE_EQ(mm_write(p, 0, written, sizeof(written)), (int64_t)sizeof(written));

  mm_block *hp = mm_block_of(p);
  REQUIRE_NOT_NULL(hp);
  mm_bin_remove(hp);

  // The payload is intact and still passes its own checksum.
  uint8_t back[32] = {0};
  CHECK_EQ(mm_read(p, 0, back, sizeof(back)), (int64_t)sizeof(back));
  CHECK_MEM_EQ(back, written, sizeof(written));
  CHECK_EQ(mm_verify(p), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(p);
  free(heap);
}

// mm_check_heap holds the bins up against the tiling. Both directions of
// disagreement have to be caught, or the check would pass a heap that has
// quietly lost a bin's worth of memory.
MM_TEST(freelist, the_heap_check_catches_bins_disagreeing_with_the_tiling) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  // 1. The bitmap claims a bin that has no head.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  {
    void *p = mm_malloc(128);
    REQUIRE_NOT_NULL(p);
    REQUIRE_EQ(mm_check_heap(), MM_OK);
    size_t empty = 0;
    while (empty < MM_BIN_COUNT && g_arena.bins[empty] != NULL) empty++;
    REQUIRE_TRUE(empty < MM_BIN_COUNT);
    g_arena.bin_bitmap[empty / 64] |= (uint64_t)1 << (empty % 64);
    CHECK_EQ(mm_check_heap(), MM_ERR_CORRUPT_LINKS);
  }

  // 2. An allocated block sitting in a bin. No bin may ever name one.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  {
    void *p[4];
    for (int i = 0; i < 4; i++) {
      p[i] = mm_malloc(128);
      REQUIRE_NOT_NULL(p[i]);
    }
    mm_free(p[1]);
    REQUIRE_EQ(mm_check_heap(), MM_OK);
    g_arena.bins[bin_of_ptr(p[1])] = mm_block_of(p[0]);
    CHECK_EQ(mm_check_heap(), MM_ERR_CORRUPT_LINKS);
  }

  // 3. A free block filed under the wrong class.
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  {
    void *p[4];
    for (int i = 0; i < 4; i++) {
      p[i] = mm_malloc(128);
      REQUIRE_NOT_NULL(p[i]);
    }
    mm_free(p[1]);
    REQUIRE_EQ(mm_check_heap(), MM_OK);
    size_t bin = bin_of_ptr(p[1]);
    mm_block *held = g_arena.bins[bin];
    g_arena.bins[bin] = NULL;
    g_arena.bin_bitmap[bin / 64] &= ~((uint64_t)1 << (bin % 64));
    size_t wrong = bin + 1;
    REQUIRE_TRUE(wrong < MM_BIN_COUNT);
    REQUIRE_TRUE(g_arena.bins[wrong] == NULL);
    g_arena.bins[wrong] = held;
    g_arena.bin_bitmap[wrong / 64] |= (uint64_t)1 << (wrong % 64);
    CHECK_EQ(mm_check_heap(), MM_ERR_CORRUPT_LINKS);
  }

  free(heap);
}

// ---------------------------------------------------------------------------
// The patrol

// Over enough calls the patrol must reach every block: a cursor that failed to
// advance, or that restarted every time, would leave most of the arena
// permanently unvisited and the whole tier would be for show.
MM_TEST(freelist, the_patrol_reaches_every_block_over_enough_calls) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_scrub_interval(0, 0);  // only the explicit calls below, nothing else

  enum { N = 24 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    p[i] = mm_malloc(96);
    REQUIRE_NOT_NULL(p[i]);
  }

  // How many blocks the tiling actually holds, counted the same way the patrol
  // walks it.
  size_t blocks = 0;
  for (uint8_t *q = g_arena.lo; q < g_arena.hi;) {
    mm_block *b = (mm_block *)(void *)q;
    REQUIRE_TRUE(mm_header_ok(b));
    blocks++;
    q += mm_block_size(b);
  }
  REQUIRE_TRUE(blocks > 4);

#ifdef MM_STATS
  mm_stats_reset();
#endif

  // One block per call, checking after each that the cursor advanced by
  // exactly that block's extent. Landing back on hi after `blocks` calls means
  // every block was visited and none was visited twice.
  uint8_t *at = g_arena.scrub_at;
  CHECK_PTR_EQ(at, g_arena.lo);
  for (size_t i = 0; i < blocks; i++) {
    size_t n = mm_block_size((mm_block *)(void *)at);
    CHECK_EQ(mm_scrub(1), MM_OK);
    at += n;
    CHECK_PTR_EQ(g_arena.scrub_at, at);
  }
  CHECK_PTR_EQ(g_arena.scrub_at, g_arena.hi);

  // And the lap starts again rather than the patrol stopping at the end.
  size_t first = mm_block_size((mm_block *)(void *)g_arena.lo);
  CHECK_EQ(mm_scrub(1), MM_OK);
  CHECK_PTR_EQ(g_arena.scrub_at, g_arena.lo + first);

#ifdef MM_STATS
  mm_stats_t st;
  mm_stats_get(&st);
  CHECK_EQ(st.scrub_passes, blocks + 1);
  CHECK_EQ(st.scrub_blocks, blocks + 1);
  CHECK_EQ(st.scrub_detections, 0);
#endif

  for (int i = 0; i < N; i++) mm_free(p[i]);
  mm_set_scrub_interval(1024, 16);
  free(heap);
}

#if MM_HAS_CANARY
// The point of the whole tier: damage to a block nobody is touching. Nothing
// here allocates, frees, reads or writes the victim after it is hit, so the
// only thing that can find it is the patrol.
MM_TEST(freelist, the_patrol_finds_damage_nothing_else_would_touch) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_scrub_interval(0, 0);

  enum { N = 24 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    p[i] = mm_malloc(96);
    REQUIRE_NOT_NULL(p[i]);
    uint8_t v = (uint8_t)i;
    REQUIRE_EQ(mm_write(p[i], 0, &v, 1), 1);
  }

  // Overrun the victim's canary by one byte, well away from the start of the
  // arena so the patrol has to walk to find it.
  void *victim = p[N - 3];
  uint8_t *past = (uint8_t *)victim + 96;
  *past ^= 0x20;

#ifdef MM_STATS
  mm_stats_reset();
#endif

  // Nothing touches the victim from here on. Patrol until it turns up.
  mm_status_t seen = MM_OK;
  for (int i = 0; i < 200 && seen == MM_OK; i++) seen = mm_scrub(2);
  CHECK_EQ(seen, MM_ERR_CORRUPT_CANARY);

  // Found means acted on: the block is quarantined, its span is accounted for,
  // and the arena still tiles.
  CHECK_EQ(mm_verify(victim), MM_ERR_QUARANTINED);
  CHECK_TRUE(g_arena.lost_bytes > 0);
  CHECK_EQ(mm_check_heap(), MM_OK);

#ifdef MM_STATS
  mm_stats_t st;
  mm_stats_get(&st);
  CHECK_TRUE(st.scrub_detections > 0);
#endif

  for (int i = 0; i < N; i++) {
    if (p[i] != victim) mm_free(p[i]);
  }
  mm_set_scrub_interval(1024, 16);
  free(heap);
}
#endif  // MM_HAS_CANARY

#if MM_HAS_CRC
// A payload that no longer matches its checksum is reported and left alone.
// The patrol was not asked for those bytes, and the API permits writing
// through the returned pointer -- so destroying a live block on the strength
// of a checksum the caller never promised to maintain would be the wrong
// trade. mm_read still refuses the block, which is where it matters.
MM_TEST(freelist, the_patrol_reports_a_bad_payload_without_taking_the_block) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_scrub_interval(0, 0);

  enum { N = 12 };
  void *p[N];
  for (int i = 0; i < N; i++) {
    p[i] = mm_malloc(64);
    REQUIRE_NOT_NULL(p[i]);
    uint8_t buf[64];
    memset(buf, (int)i + 1, sizeof(buf));
    REQUIRE_EQ(mm_write(p[i], 0, buf, sizeof(buf)), 64);
  }

  void *victim = p[N - 2];
  ((uint8_t *)victim)[7] ^= 0x01;

  mm_status_t seen = MM_OK;
  for (int i = 0; i < 200 && seen == MM_OK; i++) seen = mm_scrub(2);
  CHECK_EQ(seen, MM_ERR_CORRUPT_PAYLOAD);

  CHECK_EQ(g_arena.lost_bytes, 0);
  CHECK_FALSE(mm_is_quarantined(mm_block_of(victim)));
  CHECK_EQ(mm_check_heap(), MM_OK);

  for (int i = 0; i < N; i++) mm_free(p[i]);
  mm_set_scrub_interval(1024, 16);
  free(heap);
}
#endif  // MM_HAS_CRC

#ifdef MM_STATS
// The patrol runs by itself every N calls, and setting N to 0 stops it. Both
// halves matter: the automatic one is what makes cold memory covered at all,
// and turning it off is a trade a caller is allowed to make.
MM_TEST(freelist, the_interval_controls_whether_the_patrol_runs_at_all) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  mm_set_scrub_interval(0, 0);
  mm_stats_reset();
  for (int i = 0; i < 64; i++) {
    void *q = mm_malloc(64);
    mm_free(q);
  }
  mm_stats_t off;
  mm_stats_get(&off);
  CHECK_EQ(off.scrub_passes, 0);

  mm_set_scrub_interval(4, 8);
  mm_stats_reset();
  for (int i = 0; i < 64; i++) {
    void *q = mm_malloc(64);
    mm_free(q);
  }
  mm_stats_t on;
  mm_stats_get(&on);
  // 128 calls at one patrol every four of them.
  CHECK_EQ(on.scrub_passes, 32);
  CHECK_TRUE(on.scrub_blocks > 0);
  CHECK_EQ(on.scrub_detections, 0);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_set_scrub_interval(1024, 16);
  free(heap);
}
#endif  // MM_STATS

// The cursor is an address, and merging blocks is what can stop that address
// being a block boundary. A cursor left dangling in the middle of a merged
// block would make the next patrol walk nonsense.
MM_TEST(freelist, the_cursor_survives_blocks_merging_underneath_it) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);
  mm_set_scrub_interval(0, 0);

  void *a = mm_malloc(200);
  void *b = mm_malloc(200);
  void *c = mm_malloc(200);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(b);
  REQUIRE_NOT_NULL(c);

  // Park the cursor exactly on b, then dissolve b into a.
  g_arena.scrub_at = (uint8_t *)(void *)mm_block_of(b);
  mm_free(b);
  mm_free(a);
  CHECK_PTR_EQ(g_arena.scrub_at, (uint8_t *)(void *)mm_block_of(a));

  CHECK_EQ(mm_scrub(4), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(c);
  mm_set_scrub_interval(1024, 16);
  free(heap);
}
