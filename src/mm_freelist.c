// Size-classed free lists: the bitmap, the hardened unlink, and the search.

#include "mm_internal.h"

// --- The bitmap ------------------------------------------------------------

static void bitmap_set(size_t bin) {
  g_arena.bin_bitmap[bin / 64] |= (uint64_t)1 << (bin % 64);
}

static void bitmap_clear(size_t bin) {
  g_arena.bin_bitmap[bin / 64] &= ~((uint64_t)1 << (bin % 64));
}

static bool bitmap_test(size_t bin) {
  return (g_arena.bin_bitmap[bin / 64] >> (bin % 64)) & 1u;
}

static size_t lowest_bit(uint64_t w) {
#if defined(__GNUC__) || defined(__clang__)
  return (size_t)__builtin_ctzll(w);
#else
  size_t k = 0;
  while ((w & 1u) == 0) {
    w >>= 1;
    k++;
  }
  return k;
#endif
}

// The smallest non-empty bin at or above `from`, or MM_BIN_COUNT. This is the
// whole reason for the bitmap: it skips runs of empty bins without looking at
// them, which is what keeps the search independent of how many size classes
// there are.
static size_t next_nonempty(size_t from) {
  if (from >= MM_BIN_COUNT) return MM_BIN_COUNT;

  size_t word = from / 64;
  // Mask off the bins below `from` in its own word. `from % 64 == 0` shifts by
  // zero, which is defined; shifting by 64 would not be.
  uint64_t w = g_arena.bin_bitmap[word] & (~(uint64_t)0 << (from % 64));
  for (;;) {
    if (w != 0) return word * 64 + lowest_bit(w);
    if (++word >= MM_BIN_WORDS) return MM_BIN_COUNT;
    w = g_arena.bin_bitmap[word];
  }
}

// --- Validation ------------------------------------------------------------

// Whether `b` may be dereferenced as a member of `bin`. The order matters and
// is the point of the function: nothing is read out of the block until its
// address has been range-checked, and nothing is believed until the checksum
// has vouched for it.
static bool node_ok(const mm_block *b, size_t bin) {
  if (!mm_is_block(b)) return false;   // in the arena, aligned, sane extent
  if (!mm_header_ok(b)) return false;  // and it checksums
  // Quarantined blocks report as in use, so they are excluded here without
  // needing a case of their own.
  if (mm_is_used(b)) return false;
  return mm_bin_of(mm_block_size(b)) == bin;
}

// The same, plus the two back-links that should point at `b`. A node whose
// neighbours do not corroborate it is never spliced through: that is how a
// damaged free list turns into a wild write.
static bool links_ok(const mm_block *b, size_t bin) {
  if (!node_ok(b, bin)) return false;

  uint64_t s = g_arena.secret;
  const mm_block *next = (const mm_block *)mm_free_link_get(b, MM_LINK_NEXT, s);
  const mm_block *prev = (const mm_block *)mm_free_link_get(b, MM_LINK_PREV, s);

  if (next != NULL) {
    if (!node_ok(next, bin)) return false;
    if (mm_free_link_get(next, MM_LINK_PREV, s) != b) return false;
  }
  if (prev != NULL) {
    if (!node_ok(prev, bin)) return false;
    if (mm_free_link_get(prev, MM_LINK_NEXT, s) != b) return false;
  } else if (g_arena.bins[bin] != b) {
    return false;  // only the head of a bin has no predecessor
  }
  return true;
}

// --- Insert and remove -----------------------------------------------------

// Pushes onto the head of a bin whose head is already known to be sound --
// used by the rebuild, which is building the heads itself and must not be able
// to recurse back into another rebuild.
static void bin_push(mm_block *b, size_t bin) {
  uint64_t s = g_arena.secret;
  mm_block *head = g_arena.bins[bin];

  mm_free_link_set(b, MM_LINK_NEXT, head, s);
  mm_free_link_set(b, MM_LINK_PREV, NULL, s);
  if (head != NULL) mm_free_link_set(head, MM_LINK_PREV, b, s);
  g_arena.bins[bin] = b;
  bitmap_set(bin);
}

void mm_bins_reset(void) {
  for (size_t i = 0; i < MM_BIN_COUNT; i++) g_arena.bins[i] = NULL;
  for (size_t i = 0; i < MM_BIN_WORDS; i++) g_arena.bin_bitmap[i] = 0;
}

void mm_bins_rebuild(void) {
  mm_bins_reset();

  size_t budget = mm_max_blocks();
  for (uint8_t *p = g_arena.lo; p < g_arena.hi;) {
    if (budget-- == 0) break;
    mm_block *b = (mm_block *)(void *)p;
    // Without a legible header there is no way to step past this block, so the
    // rebuild stops here rather than guessing. Free blocks beyond it drop out
    // of the bins; they are still in the tiling, and mm_check_heap says so.
    if (!mm_header_ok(b)) break;

    if (!mm_is_used(b)) bin_push(b, mm_bin_of(mm_block_size(b)));
    p += mm_block_size(b);
  }
}

void mm_bin_insert(mm_block *b) {
  size_t bin = mm_bin_of(mm_block_size(b));
  mm_block *head = g_arena.bins[bin];

  if (head != NULL && !node_ok(head, bin)) {
    // The bin cannot be extended through a head that no longer stands up. `b`
    // is already sealed and marked free, so a rebuild picks it up.
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_bins_rebuild();
    return;
  }

  bin_push(b, bin);
}

void mm_bin_remove(mm_block *b) {
  size_t bin = mm_bin_of(mm_block_size(b));
  uint64_t s = g_arena.secret;

  if (!links_ok(b, bin)) {
    // The tiling is the authority, so rebuild from it and try once more.
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_bins_rebuild();
    if (!links_ok(b, bin)) {
      // The rebuild could not reach `b`, so there is no list to take it off.
      // Its links are cleared only if the space they sit in is still the
      // block's own to write: scribbling over an allocated block's payload to
      // tidy up a list it was never on would destroy the caller's data to no
      // purpose whatsoever.
      if (!mm_is_used(b)) {
        mm_free_link_set(b, MM_LINK_NEXT, NULL, s);
        mm_free_link_set(b, MM_LINK_PREV, NULL, s);
      }
      return;
    }
  }

  mm_block *next = (mm_block *)mm_free_link_get(b, MM_LINK_NEXT, s);
  mm_block *prev = (mm_block *)mm_free_link_get(b, MM_LINK_PREV, s);

  if (prev != NULL) {
    mm_free_link_set(prev, MM_LINK_NEXT, next, s);
  } else {
    g_arena.bins[bin] = next;
    if (next == NULL) bitmap_clear(bin);
  }
  if (next != NULL) mm_free_link_set(next, MM_LINK_PREV, prev, s);
}

// --- Search ----------------------------------------------------------------

// First block in `bin` holding at least `need` bytes, looking at no more than
// MM_BIN_SCAN_CAP of them. Sets `*damaged` and gives up if a node does not
// corroborate the one before it.
static mm_block *scan_bin(size_t bin, size_t need, bool *damaged) {
  uint64_t s = g_arena.secret;
  mm_block *back = NULL;
  mm_block *cur = g_arena.bins[bin];

  for (size_t seen = 0; cur != NULL && seen < MM_BIN_SCAN_CAP; seen++) {
    if (!node_ok(cur, bin) ||
        mm_free_link_get(cur, MM_LINK_PREV, s) != back) {
      *damaged = true;
      return NULL;
    }
    if (mm_block_size(cur) >= need) return cur;
    back = cur;
    cur = (mm_block *)mm_free_link_get(cur, MM_LINK_NEXT, s);
  }
  return NULL;
}

// The head of `bin`, which fits by construction because every block in a bin
// above the requested one is strictly larger. Validated all the same.
static mm_block *bin_head(size_t bin, bool *damaged) {
  mm_block *h = g_arena.bins[bin];
  if (h == NULL || !node_ok(h, bin) ||
      mm_free_link_get(h, MM_LINK_PREV, g_arena.secret) != NULL) {
    *damaged = true;  // the bitmap and the bin disagree, or the head is junk
    return NULL;
  }
  return h;
}

mm_block *mm_bin_find(size_t need) {
  // Two attempts at most: the second runs against bins rebuilt from the
  // tiling, and if that still does not stand up there is nothing left to try.
  for (int attempt = 0; attempt < 2; attempt++) {
    bool damaged = false;
    size_t bin = mm_bin_of(need);

    // The bin the request itself lands in is the only one that can hold
    // blocks too small for it, so it is the only one that gets scanned. For an
    // exact class the first node is already the answer.
    mm_block *b = scan_bin(bin, need, &damaged);
    if (b != NULL) return b;

    // The smallest non-empty bin above it, whose every member is strictly
    // larger and therefore fits. There is no "try the next one after that":
    // a head that cannot be used means the bins disagree with themselves, and
    // the answer to that is a rebuild, not another probe.
    if (!damaged) {
      size_t j = next_nonempty(bin + 1);
      if (j < MM_BIN_COUNT) {
        b = bin_head(j, &damaged);
        if (b != NULL) return b;
      }
    }
    if (!damaged) return NULL;

    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_bins_rebuild();
  }
  return NULL;
}

// --- Consistency -----------------------------------------------------------

mm_status_t mm_bins_check(const size_t *counts) {
  uint64_t s = g_arena.secret;

  for (size_t i = 0; i < MM_BIN_COUNT; i++) {
    // The bitmap is a cache over bin emptiness and has to say the same thing.
    // Getting this wrong is silent -- a stale bit costs a wasted probe, a
    // missing one loses the memory in the bin -- so it is checked, not assumed.
    if (bitmap_test(i) != (g_arena.bins[i] != NULL)) {
      return mm_fail(MM_ERR_CORRUPT_LINKS);
    }

    size_t seen = 0;
    size_t budget = mm_max_blocks();
    mm_block *back = NULL;
    for (mm_block *cur = g_arena.bins[i]; cur != NULL;) {
      if (budget-- == 0) return mm_fail(MM_ERR_CORRUPT_LINKS);
      if (!mm_header_ok(cur)) return mm_fail(MM_ERR_CORRUPT_LINKS);
      if (mm_is_used(cur)) return mm_fail(MM_ERR_CORRUPT_LINKS);
      if (mm_bin_of(mm_block_size(cur)) != i) {
        return mm_fail(MM_ERR_CORRUPT_LINKS);
      }
      if (mm_free_link_get(cur, MM_LINK_PREV, s) != back) {
        return mm_fail(MM_ERR_CORRUPT_LINKS);
      }
      back = cur;
      cur = (mm_block *)mm_free_link_get(cur, MM_LINK_NEXT, s);
      seen++;
    }
    // Every back-link was checked, so a block cannot appear twice in one bin;
    // every member was checked to belong to this bin, so it cannot be in a
    // second one; and this makes the count right. Together: exactly one bin
    // for every free block, and no bin at all for an allocated one.
    if (seen != counts[i]) return mm_fail(MM_ERR_CORRUPT_LINKS);
  }
  return MM_OK;
}
