// Size-classed free lists.
//
// Allocation used to be a linear walk of the free list looking for the
// smallest sufficient block, revalidating every node on the way. That walk was
// the whole cost of allocating: the `fragmentation` workload spent a 600 us
// median inside it. This replaces it with bins, so that finding a block is a
// bit-scan and a pointer read.
//
// --- How the sizes are cut -------------------------------------------------
//
// Bins 0..63 are **exact classes**: bin i holds blocks of exactly
// MM_MIN_BLOCK + 16i, which covers every request up to about a kilobyte. An
// exact class needs no search whatsoever -- the head of the bin is the answer
// -- and the list is kept in LIFO order because the block freed most recently
// is the one most likely to still be in cache.
//
// Bins 64..127 are **log-spaced**, four sub-bins per octave from 2^10 upwards,
// so a bin spans a quarter of an octave rather than a whole one. Within such a
// bin the blocks differ in size, so there is a scan -- capped at
// MM_BIN_SCAN_CAP. That makes it a "good fit" rather than a best fit, which is
// the deliberate trade: an unbounded best-fit search is exactly the cost this
// header exists to remove.
//
// Sixty-four large bins reach 2^26 with the top one catching everything above,
// and 64 + 64 bins is exactly the 128 bits of `bin_bitmap`. (The plan called
// for bins 64..95 covering 1 KB to 16 MB at four per octave; those three
// numbers cannot all hold at once -- fourteen octaves at four bins each need
// 56 bins, not 32 -- so the bin count gave, since it is the one of the three
// with no consequence outside this file.)
//
// --- Why the search is O(1) ------------------------------------------------
//
// mm_bin_of is monotonic non-decreasing in the block size. That single
// property is what makes the search cheap: every bin above the one a request
// maps to can hold only blocks that are strictly larger, so its head fits
// without being examined. Only the bin the request itself lands in has to be
// scanned, and that scan is capped. `bin_bitmap` then finds the smallest
// non-empty bin above it with a mask and a count-trailing-zeros, so empty bins
// cost nothing at all.
//
// --- Why unlinking is paranoid ---------------------------------------------
//
// The links live in the free block's payload space (XOR-ed with the arena
// secret, see mm_layout.h) and no checksum covers them. The free list is the
// single most attacked structure in any allocator, and `links` is already a
// fault-injection target. So every node is validated before it is
// dereferenced, in the order glibc settled on after 2015: in the arena and
// aligned, then its checksum, then free and in *this* bin, and only then the
// back-links. Anything that disagrees causes the bins to be rebuilt from the
// tiling -- which is the authority -- and reported as MM_ERR_CORRUPT_LINKS.

#ifndef MARS_MM_FREELIST_H_
#define MARS_MM_FREELIST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mars/allocator.h"
#include "mm_layout.h"

// --- Geometry of the bins --------------------------------------------------

#define MM_SMALL_BINS ((size_t)64)
#define MM_LARGE_BINS ((size_t)64)
#define MM_BIN_COUNT (MM_SMALL_BINS + MM_LARGE_BINS)
#define MM_BIN_WORDS (MM_BIN_COUNT / 64)

// The first size too large for an exact class.
#define MM_LARGE_MIN (MM_MIN_BLOCK + MM_ALIGNMENT * MM_SMALL_BINS)

// The octave bin 64 starts from, and how finely each octave is cut.
#define MM_LARGE_OCTAVE0 10
#define MM_LARGE_SUBBINS ((size_t)4)

// How many blocks a scan of a log-spaced bin will look at before giving up on
// it and moving to a larger one.
#define MM_BIN_SCAN_CAP ((size_t)16)

_Static_assert(MM_LARGE_MIN >= ((size_t)1 << MM_LARGE_OCTAVE0) &&
                   MM_LARGE_MIN < ((size_t)1 << (MM_LARGE_OCTAVE0 + 1)),
               "the exact classes must stop inside the octave the log-spaced "
               "bins start from, or sizes fall between the two schemes");
_Static_assert(MM_LARGE_BINS % MM_LARGE_SUBBINS == 0,
               "the large bins must be a whole number of octaves");
_Static_assert(MM_BIN_COUNT % 64 == 0, "the bitmap is a whole number of words");

// --- Size to bin -----------------------------------------------------------

static inline int mm_floor_log2(uint64_t n) {
#if defined(__GNUC__) || defined(__clang__)
  return 63 - __builtin_clzll(n);
#else
  int k = 0;
  while ((n >>= 1) != 0) k++;
  return k;
#endif
}

// Which bin a block of `n` bytes belongs in.
//
// Monotonic non-decreasing in `n`, including across the join between the two
// schemes and at the clamp on the top bin. tests/test_freelist.c checks that
// over every 16-granular size the arena can hold, because the whole search
// rests on it.
static inline size_t mm_bin_of(size_t n) {
  if (n < MM_LARGE_MIN) {
    if (n <= MM_MIN_BLOCK) return 0;
    return (n - MM_MIN_BLOCK) / MM_ALIGNMENT;
  }
  int k = mm_floor_log2((uint64_t)n);
  // The two bits below the leading one choose the quarter of the octave.
  size_t sub = (size_t)(((uint64_t)n >> (k - 2)) & 3u);
  size_t idx = MM_SMALL_BINS +
               (size_t)(k - MM_LARGE_OCTAVE0) * MM_LARGE_SUBBINS + sub;
  return idx < MM_BIN_COUNT ? idx : MM_BIN_COUNT - 1;
}

// --- Operations ------------------------------------------------------------

// Empties every bin and the bitmap with it. Does not touch the blocks.
void mm_bins_reset(void);

// Rebuilds the bins from the implicit list, which is the authority. Called
// whenever a traversal finds links that do not corroborate each other.
void mm_bins_rebuild(void);

// Puts a free block into its bin, at the head. `b` must already carry a
// sealed, free control word, so that a rebuild triggered from inside this call
// still finds a consistent arena.
void mm_bin_insert(mm_block *b);

// Takes a free block out of its bin, validating before it dereferences
// anything. Safe to call on a block that is not on any list.
void mm_bin_remove(mm_block *b);

// A free block of at least `need` bytes, still in its bin, or NULL. `need`
// must be a whole number of alignment units and at least MM_MIN_BLOCK.
mm_block *mm_bin_find(size_t need);

// Holds the bins up against what a walk of the implicit list found:
// `counts[i]` is how many free blocks of class i that walk saw. Checks that
// the bitmap agrees with bin emptiness, that every member of a bin is free and
// in the right bin, that the back-links agree, and that the totals match --
// which together mean every free block is in exactly one bin and no allocated
// block is in any.
mm_status_t mm_bins_check(const size_t *counts);

#endif  // MARS_MM_FREELIST_H_
