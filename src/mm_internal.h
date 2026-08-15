// Internals shared between the allocator's translation units. Not installed.

#ifndef MARS_MM_INTERNAL_H_
#define MARS_MM_INTERNAL_H_

#include "mars/allocator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm_freelist.h"
#include "mm_layout.h"

// --- Arena state -----------------------------------------------------------
//
// Blocks tile [lo, hi) exactly, always -- there is no list to fall out of step
// with the tiling, because the tiling *is* the list. Stepping forward is
// `b + block_size`; stepping backward over a free neighbour is its boundary
// tag. Quarantined blocks stay in the tiling as permanently-allocated ones
// nobody owns, which is why lost space can be checked rather than assumed.

typedef struct mm_arena {
  uint8_t *base;
  size_t size;
  uint8_t *lo;  // first block; base plus MM_BLOCK_OFFSET
  uint8_t *hi;  // one past the last block
  // Heads of the size-classed free lists, and one bit per non-empty bin so
  // that the smallest sufficient one is found with a mask and a bit-scan
  // rather than by looking. See mm_freelist.h for how the sizes are cut.
  mm_block *bins[MM_BIN_COUNT];
  uint64_t bin_bitmap[MM_BIN_WORDS];
  // Drawn once per arena and mixed into every checksum, canary and free-list
  // link. A corruption detector, not a security boundary.
  uint64_t secret;
  // Space belonging to quarantined blocks. Deliberately never reclaimed, but
  // tracked, so that the consistency check can tell the difference between
  // memory that was given up on purpose and memory that has gone missing.
  size_t lost_bytes;
  // Where the next patrol resumes. Always a block boundary: anything that
  // makes it interior to a block moves it back, through mm_scrub_forget.
  uint8_t *scrub_at;
} mm_arena;

extern mm_arena g_arena;

// Records the calling thread's status and returns it, so call sites can write
// `return mm_fail(MM_ERR_OOB);`.
mm_status_t mm_fail(mm_status_t status);
void mm_clear_error(void);

// --- Geometry helpers ------------------------------------------------------

static inline size_t mm_round_up(size_t n, size_t to) {
  return (n + to - 1) & ~(to - 1);
}

// Block size needed to satisfy a request of `payload` bytes, or 0 if the
// computation would overflow.
size_t mm_size_for(size_t payload);

// Position of a block within the arena, in alignment units. Mixed into the
// checksum and the canary so that both detect a block being confused with
// another one, not merely a bit being flipped inside it.
static inline uint64_t mm_block_index(const mm_block *b) {
  return (uint64_t)(((const uint8_t *)(const void *)b - g_arena.lo) /
                    MM_ALIGNMENT);
}

// True if `b` could be a block header: aligned to the tiling, inside the
// arena, carrying a sane extent that does not run past the end.
bool mm_is_block(const mm_block *b);

// --- Integrity (mm_integrity.c) -------------------------------------------

// Structure plus checksum. Under the fast profile there is no checksum, so
// this is the structural check alone and says so honestly.
bool mm_header_ok(const mm_block *b);

bool mm_canary_ok(const mm_block *b);
void mm_write_canary(mm_block *b);

// Recomputes the header checksum and republishes the tail mirror, if the
// profile has one. Every write to a header ends here.
void mm_seal(mm_block *b);

// Establishes PREV_IN_USE on the block after `b`, and, when `b` is free, its
// boundary tag. Called after anything that changes `b`'s extent or state.
void mm_publish(mm_block *b);

// Gives up on a block: it stays in the tiling so the arena still tiles, but
// it is marked quarantined, never merged and never handed out again. A block
// whose header cannot be trusted is routed through mm_rescue instead, since
// its own extent is the thing in doubt.
void mm_quarantine(mm_block *b);

// Tries to make a block with an untrustworthy header trustworthy again.
//
// Returns true when the header now stands up -- only possible where the
// profile carries a tail mirror. Otherwise the block's span is surrendered as
// a quarantined block and false is returned. Either way the arena still tiles
// when this returns.
bool mm_rescue(mm_block *b);

// The block preceding `b`, reached through its boundary tag, or NULL when
// there is none, when PREV_IN_USE forbids looking, or when what is there does
// not corroborate itself.
mm_block *mm_prev_free_block(const mm_block *b);

// Recovers a block header from a payload pointer, validating as it goes.
// Returns NULL and sets the thread status if the pointer is not ours.
mm_block *mm_block_of(const void *ptr);

// Upper bound on how many blocks the arena can hold; bounds every traversal so
// that a corrupted link cannot produce an unbounded loop.
size_t mm_max_blocks(void);

// --- Scrubbing (mm_integrity.c) --------------------------------------------
//
// Binning means the allocator stops touching most of the arena: a block that
// is neither allocated nor freed nor next to something being coalesced is
// never looked at again. The linear search used to catch damage in cold memory
// incidentally, and that incidental cover is exactly what O(1) allocation
// takes away. The patrol puts it back deliberately and at a bounded cost --
// which is what a hardware ECC scrubber does on real spacecraft.

// Runs the patrol if enough operations have gone by since the last one. Called
// at the end of every public allocation call. Never clears the thread status:
// something it finds has to survive to mm_last_error().
void mm_scrub_tick(void);

// Moves the patrol cursor back to `from` if it currently sits strictly inside
// [from, to). Called wherever two blocks merge or a span is surrendered, since
// those are the only things that can make a block boundary stop being one.
void mm_scrub_forget(uint8_t *from, uint8_t *to);

// Pins the secret the next mm_init will use, so that a harness which has to
// replay a run byte for byte can. Zero restores the drawn-at-random default.
// Not part of the public API; the shipped allocator never calls it.
void mm_pin_secret(uint64_t secret);

// --- Damage recovery (mm_integrity.c) --------------------------------------

// Produces the block that should follow `owner`, whose header is trusted.
//
// When the header at that address checks out, that is the answer. When it does
// not, the walk scans forward for the next header that does, which is the only
// way back in step once an extent has been lost. Under the paranoid profile
// the tail mirror sitting immediately before that resynchronisation point
// identifies the damaged block exactly and it is rebuilt; otherwise its span
// is surrendered as a quarantined block and counted in lost_bytes.
//
// Returns NULL when nothing follows. Never returns a block it has not made
// consistent with `owner`.
mm_block *mm_recover_next(mm_block *owner);

// --- Counters (mm_stats.c) -------------------------------------------------

typedef enum mm_stats_event {
  MM_STAT_ALLOC,
  MM_STAT_ALLOC_FAILED,
  MM_STAT_FREE,
  MM_STAT_REALLOC,
  MM_STAT_QUARANTINE,
  MM_STAT_REPAIR,
  MM_STAT_SCRUB,     // one patrol; `bytes` carries the blocks it visited
  MM_STAT_SCRUB_HIT  // that patrol found something wrong
} mm_stats_event_t;

#ifdef MM_STATS
void mm_stats_block_added(size_t block_bytes, size_t payload_bytes);
void mm_stats_block_removed(size_t block_bytes, size_t payload_bytes);
void mm_stats_note(mm_stats_event_t event, size_t bytes);
#  define MM_STAT_ADD(b) \
    mm_stats_block_added(mm_block_size(b), mm_requested_size(b))
#  define MM_STAT_SUB(b) \
    mm_stats_block_removed(mm_block_size(b), mm_requested_size(b))
#  define MM_STAT_NOTE(e, n) mm_stats_note((e), (n))
#else
// Compiled away completely, so the fast path carries no trace of them.
#  define MM_STAT_ADD(b) ((void)0)
#  define MM_STAT_SUB(b) ((void)0)
#  define MM_STAT_NOTE(e, n) ((void)0)
#endif

#endif  // MARS_MM_INTERNAL_H_
