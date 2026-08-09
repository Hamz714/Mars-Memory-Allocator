// Internals shared between the allocator's translation units. Not installed.

#ifndef MARS_MM_INTERNAL_H_
#define MARS_MM_INTERNAL_H_

#include "mars/allocator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// --- Block layout ----------------------------------------------------------
//
//   +0                    header        (checksummed metadata)
//   +sizeof(header)       padding       8 bytes
//   +sizeof(header)+8     clue          8 bytes, offset back to the header
//   +MM_PREFIX            payload       requested_size bytes
//   +MM_PREFIX+requested  canary        8 bytes
//   ...                   (slack from rounding the block up to MM_ALIGNMENT)
//   +sizeof(header)+size  footer        mirror of the header
//
// header.size counts every byte between the end of the header and the start of
// the footer, and is always a multiple of MM_ALIGNMENT. Since sizeof(header) is
// too, every block begins on an MM_ALIGNMENT boundary, and so does every
// payload -- which is what keeps the header casts well-defined.

#define MM_HEADER_MAGIC 0xBADC0FFEu
#define MM_CANARY 0xCAFEBABECAFEBABEULL
#define MM_ALIGNMENT 16u

typedef struct mm_header {
  uint32_t magic;
  uint32_t in_use;
  size_t size;            // bytes between this header and its footer
  size_t requested_size;  // exact payload size the caller asked for
  struct mm_header *next;
  struct mm_header *prev;
  uint32_t checksum;          // over the metadata above
  uint32_t payload_checksum;  // over the payload bytes
} mm_header;

typedef mm_header mm_footer;

_Static_assert(sizeof(mm_header) % MM_ALIGNMENT == 0,
               "header size must be a multiple of the alignment, or blocks "
               "after the first will not be aligned");

// Distance from the start of a block to its payload: header, then 8 bytes of
// padding and the 8-byte clue that sits immediately before the payload.
#define MM_PREFIX (sizeof(mm_header) + 2 * sizeof(uint64_t))

// Bytes of overhead inside header.size that are not payload.
#define MM_BLOCK_TAX (2 * sizeof(uint64_t) + sizeof(uint64_t))

// Smallest header.size worth carving out as a separate block.
#define MM_MIN_BLOCK_SIZE ((size_t)32)

// Total footprint of a block whose header.size is n.
#define MM_BLOCK_TOTAL(n) (sizeof(mm_header) + (n) + sizeof(mm_footer))

// --- Arena state -----------------------------------------------------------

typedef struct mm_arena {
  uint8_t *base;
  size_t size;
  mm_header *first;
  // Space belonging to quarantined blocks. Deliberately never reclaimed, but
  // tracked, so that the consistency check can tell the difference between
  // memory that was given up on purpose and memory that has gone missing.
  size_t lost_bytes;
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

// header.size needed to satisfy a request of `payload` bytes, or 0 if the
// computation would overflow.
size_t mm_size_for(size_t payload);

static inline uint8_t *mm_payload_of(const mm_header *block) {
  return (uint8_t *)block + MM_PREFIX;
}

static inline mm_footer *mm_footer_of(const mm_header *block) {
  return (mm_footer *)(void *)((uint8_t *)block + sizeof(mm_header) +
                               block->size);
}

static inline mm_header *mm_next_adjacent(const mm_header *block) {
  return (mm_header *)(void *)((uint8_t *)block +
                               MM_BLOCK_TOTAL(block->size));
}

// --- Integrity (mm_integrity.c) -------------------------------------------

uint32_t mm_checksum(const mm_header *block);
uint32_t mm_payload_checksum(const void *payload, size_t len);

// True if `p` points at a plausible block header inside the arena.
bool mm_in_arena(const void *p, size_t span);
bool mm_is_block(const mm_header *block);

bool mm_checksum_ok(const mm_header *block);
bool mm_footer_ok(const mm_header *block);
bool mm_canary_ok(const mm_header *block);

void mm_read_canary(const mm_header *block, uint64_t *out);
void mm_write_canary(const mm_header *block);

// Re-derives the checksum and republishes the footer after a header changed.
void mm_seal(mm_header *block);

// Unlinks a block whose metadata can no longer be trusted.
void mm_quarantine(mm_header *block);

// Produces the block that should follow `owner`, whose metadata is trusted.
//
// Blocks tile the arena, so the following block's address is known even when
// its header is not. A header that cannot be trusted is rebuilt from its
// mirrored footer where that footer can be located and corroborated; where it
// cannot, the walk resynchronises on the next intact block. Any span that had
// to be abandoned is reported through `lost` rather than silently dropped.
mm_header *mm_recover_next(mm_header *owner, size_t *lost);

// Recovers a block header from a payload pointer, validating as it goes.
// Returns NULL and sets the thread status if the pointer is not ours.
mm_header *mm_block_of(const void *ptr);

// Upper bound on how many blocks the arena can hold; bounds every traversal so
// that a corrupted link cannot produce an unbounded loop.
size_t mm_max_blocks(void);

// --- Counters (mm_stats.c) -------------------------------------------------

typedef enum mm_stats_event {
  MM_STAT_ALLOC,
  MM_STAT_ALLOC_FAILED,
  MM_STAT_FREE,
  MM_STAT_REALLOC,
  MM_STAT_QUARANTINE,
  MM_STAT_REPAIR
} mm_stats_event_t;

#ifdef MM_STATS
void mm_stats_block_added(const mm_header *block);
void mm_stats_block_removed(const mm_header *block);
void mm_stats_note(mm_stats_event_t event, size_t bytes);
#  define MM_STAT_ADD(b) mm_stats_block_added(b)
#  define MM_STAT_SUB(b) mm_stats_block_removed(b)
#  define MM_STAT_NOTE(e, n) mm_stats_note((e), (n))
#else
// Compiled away completely, so the fast path carries no trace of them.
#  define MM_STAT_ADD(b) ((void)0)
#  define MM_STAT_SUB(b) ((void)0)
#  define MM_STAT_NOTE(e, n) ((void)0)
#endif

#endif  // MARS_MM_INTERNAL_H_
