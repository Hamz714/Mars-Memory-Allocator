// Block layout, selected at compile time by MARS_PROFILE.
//
// Everything that knows how a block is shaped lives here: the sizes, the
// offsets, the control-word encoding, and the accessors that read and write
// it. The rest of the allocator is written against those accessors, so a
// change of profile is a change of this header alone.
//
// --- What replaced what ----------------------------------------------------
//
// The previous layout carried a 48-byte checksummed header, a full 48-byte
// mirror of it as a footer, 8 bytes of padding, an 8-byte "clue" and an 8-byte
// canary on every block, allocated or free: about 128 bytes per allocation.
// Three eliminations account for almost all of that.
//
//   * The footer on allocated blocks. Boundary tags put a footer only in a
//     *free* block, inside payload space it is not using, and a PREV_IN_USE
//     bit in the following header says whether looking backwards is legal.
//     Backward coalescing stays O(1) and allocated blocks pay nothing.
//
//   * The clue. It existed so a payload pointer could be cross-checked before
//     its header was trusted. With a compile-time-constant header size and
//     structural 16-byte alignment, `hdr = ptr - MM_HDR_SIZE` unconditionally,
//     and the CRC does the cross-checking.
//
//   * The padding. Absolute 16-byte alignment, 16-granular block sizes and a
//     constant header make payload alignment structural rather than something
//     to pad towards.
//
// Losing the mirror costs no detection, and that is the argument the whole
// brief rests on: a 32-bit CRC over a 32-byte input detects every 1-, 2- and
// 3-bit error outright and misses a random one with probability 2^-32. The
// mirror was never adding detection -- it was adding *repair*, which is why
// the paranoid profile keeps a tail mirror and the other two do not.
//
// --- requested_size does not need 64 bits ----------------------------------
//
// The old header stored the caller's exact request in a size_t. It does not
// need one. A block is split whenever the remainder is at least MM_MIN_BLOCK,
// so the distance between a block's extent and the payload inside it is
// bounded by a small constant -- and only that distance, the *slack*, has to
// be stored. requested_size is then recovered exactly:
//
//     requested_size = block_size - MM_HDR_SIZE - slack
//
// MM_SLACK_MAX below derives the bound from the profile's own constants and a
// _Static_assert pins it against the field width. tests/test_layout.c checks
// the round trip for every size from 1 to 4096.

#ifndef MARS_MM_LAYOUT_H_
#define MARS_MM_LAYOUT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "mm_crc32.h"

// --- Profile ---------------------------------------------------------------

#if defined(MARS_PROFILE_FAST)
#  define MM_PROFILE_NAME "fast"
#  define MM_HAS_CRC 0
#  define MM_HAS_CANARY 0
#  define MM_HAS_MIRROR 0
#elif defined(MARS_PROFILE_PARANOID)
#  define MM_PROFILE_NAME "paranoid"
#  define MM_HAS_CRC 1
#  define MM_HAS_CANARY 1
#  define MM_HAS_MIRROR 1
#else
// Hardened is the default, including for builds that do not go through CMake.
#  ifndef MARS_PROFILE_HARDENED
#    define MARS_PROFILE_HARDENED 1
#  endif
#  define MM_PROFILE_NAME "hardened"
#  define MM_HAS_CRC 1
#  define MM_HAS_CANARY 1
#  define MM_HAS_MIRROR 0
#endif

// --- Sizes -----------------------------------------------------------------

#define MM_ALIGNMENT ((size_t)16)

#if MM_HAS_CRC
#  define MM_HDR_SIZE ((size_t)16)
#else
#  define MM_HDR_SIZE ((size_t)8)
#endif

#if MM_HAS_CANARY
#  define MM_CANARY_SIZE ((size_t)8)
#else
#  define MM_CANARY_SIZE ((size_t)0)
#endif

#if MM_HAS_MIRROR
#  define MM_MIRROR_SIZE MM_HDR_SIZE
#else
#  define MM_MIRROR_SIZE ((size_t)0)
#endif

// Bytes an allocated block spends after the payload.
#define MM_TRAIL (MM_CANARY_SIZE + MM_MIRROR_SIZE)

// Smallest block the allocator will carve. A free block has to hold its
// header, both free-list links and its boundary-tag footer; an allocated one
// has to hold its header, at least one payload byte and its trailer.
#define MM_FREE_MIN (MM_HDR_SIZE + 2 * sizeof(uint64_t) + sizeof(uint64_t))
#define MM_USED_MIN (MM_HDR_SIZE + 1 + MM_TRAIL)
#define MM_MIN_BLOCK                                                          \
  (((MM_FREE_MIN > MM_USED_MIN ? MM_FREE_MIN : MM_USED_MIN) + MM_ALIGNMENT -  \
    1) &                                                                      \
   ~(MM_ALIGNMENT - 1))

// Where the first block sits relative to the arena base. The payload, not the
// block, is what has to be 16-aligned, so a header that is not a multiple of
// the alignment shifts every block by the difference -- dlmalloc's trick, and
// the reason an 8-byte header costs 8 bytes of overhead rather than 16.
#define MM_BLOCK_OFFSET ((MM_ALIGNMENT - MM_HDR_SIZE % MM_ALIGNMENT) %        \
                         MM_ALIGNMENT)

// --- Control word ----------------------------------------------------------
//
//   bits 63..10  block_size >> 4     the block's whole extent, 16-granular
//   bits  9.. 3  slack               block_size - MM_HDR_SIZE - requested
//   bit       2  QUARANTINED         given up on; never reused, never merged
//   bit       1  PREV_IN_USE         may the preceding footer be read?
//   bit       0  IN_USE

#define MM_W_IN_USE ((uint64_t)1 << 0)
#define MM_W_PREV_IN_USE ((uint64_t)1 << 1)
#define MM_W_QUARANTINED ((uint64_t)1 << 2)
#define MM_W_SLACK_SHIFT 3
#define MM_W_SLACK_BITS 7
#define MM_W_SLACK_MASK (((uint64_t)1 << MM_W_SLACK_BITS) - 1)
#define MM_W_SIZE_SHIFT (MM_W_SLACK_SHIFT + MM_W_SLACK_BITS)
#define MM_W_FLAGS_MASK (((uint64_t)1 << MM_W_SIZE_SHIFT) - 1)

// The slack a block carries the moment it is carved, before any splitting:
// either the trailer plus at most 15 bytes of rounding, or -- for requests too
// small to reach MM_MIN_BLOCK -- whatever the floor leaves over a 1-byte one.
#define MM_SLACK_ROUNDING (MM_TRAIL + MM_ALIGNMENT - 1)
#define MM_SLACK_FLOOR (MM_MIN_BLOCK - MM_HDR_SIZE - 1)
#define MM_SLACK_CARVED                                                       \
  (MM_SLACK_FLOOR > MM_SLACK_ROUNDING ? MM_SLACK_FLOOR : MM_SLACK_ROUNDING)

// On top of that, a block keeps any remainder too small to split off, which is
// a multiple of the alignment and strictly below MM_MIN_BLOCK.
#define MM_SLACK_MAX (MM_SLACK_CARVED + MM_MIN_BLOCK - MM_ALIGNMENT)

// --- The header ------------------------------------------------------------

typedef struct mm_block {
  uint64_t word;
#if MM_HAS_CRC
  // Over the control word, the payload CRC, the block's index within the
  // arena and the arena's secret. Including the index and the secret is what
  // makes this detect *block confusion* rather than only bit flips: a header
  // copied verbatim from elsewhere in the arena fails here.
  uint32_t hdr_crc;
  // Over the payload, lazily maintained. 0 means "not established", a value
  // mm_payload_crc never returns.
  uint32_t payload_crc;
#endif
} mm_block;

// The value stamped past the payload, before it is bound to a block.
#define MM_CANARY 0xCAFEBABECAFEBABEULL

// --- Static assertions -----------------------------------------------------

_Static_assert(sizeof(mm_block) == MM_HDR_SIZE,
               "the header struct and MM_HDR_SIZE must agree, or every offset "
               "derived from MM_HDR_SIZE is wrong");
_Static_assert(MM_HDR_SIZE % sizeof(uint64_t) == 0,
               "the header must be 8-aligned so the control word is aligned");
_Static_assert((MM_BLOCK_OFFSET + MM_HDR_SIZE) % MM_ALIGNMENT == 0,
               "payloads must land on MM_ALIGNMENT; that is what makes the "
               "alignment structural rather than something to pad towards");
_Static_assert(MM_MIN_BLOCK % MM_ALIGNMENT == 0,
               "block sizes are 16-granular, floor included");
_Static_assert(MM_MIN_BLOCK >= MM_FREE_MIN,
               "a free block must hold two links and a footer");
_Static_assert(MM_MIN_BLOCK >= MM_USED_MIN,
               "an allocated block must hold a byte of payload and a trailer");
_Static_assert(MM_SLACK_MAX <= MM_W_SLACK_MASK,
               "the slack field is too narrow for this profile's geometry; "
               "requested_size would not round-trip");
_Static_assert(MM_TRAIL == MM_CANARY_SIZE + MM_MIRROR_SIZE,
               "the trailer is the canary and the mirror and nothing else");
_Static_assert(MM_MIRROR_SIZE == 0 || MM_MIRROR_SIZE == MM_HDR_SIZE,
               "the mirror is a copy of the header or it is absent");
#if MM_HAS_CRC
_Static_assert(offsetof(mm_block, hdr_crc) == 8, "hdr_crc sits at +8");
_Static_assert(offsetof(mm_block, payload_crc) == 12, "payload_crc sits at +12");
#endif
// --- Control-word accessors ------------------------------------------------

static inline size_t mm_block_size(const mm_block *b) {
  return (size_t)((b->word >> MM_W_SIZE_SHIFT) * MM_ALIGNMENT);
}

static inline void mm_set_block_size(mm_block *b, size_t n) {
  b->word = (b->word & MM_W_FLAGS_MASK) |
            ((uint64_t)(n / MM_ALIGNMENT) << MM_W_SIZE_SHIFT);
}

static inline bool mm_is_used(const mm_block *b) {
  return (b->word & MM_W_IN_USE) != 0;
}

static inline bool mm_prev_in_use(const mm_block *b) {
  return (b->word & MM_W_PREV_IN_USE) != 0;
}

static inline bool mm_is_quarantined(const mm_block *b) {
  return (b->word & MM_W_QUARANTINED) != 0;
}

static inline void mm_set_prev_in_use(mm_block *b, bool used) {
  b->word = used ? (b->word | MM_W_PREV_IN_USE)
                 : (b->word & ~MM_W_PREV_IN_USE);
}

static inline size_t mm_slack(const mm_block *b) {
  return (size_t)((b->word >> MM_W_SLACK_SHIFT) & MM_W_SLACK_MASK);
}

// Exactly what the caller asked for. Meaningful only while the block is in
// use; a free block carries no request and reports 0.
//
// Clamped rather than trusted. Under a profile with a checksum the slack has
// already been vouched for by the time this is called, but under `fast` it has
// not, and a flipped slack bit that underflowed this subtraction would produce
// a payload extent reaching past the arena. The clamp keeps every payload
// inside its own block whatever the control word says.
static inline size_t mm_requested_size(const mm_block *b) {
  if (!mm_is_used(b)) return 0;
  size_t avail = mm_block_size(b) - MM_HDR_SIZE;
  size_t slack = mm_slack(b);
  return slack <= avail ? avail - slack : 0;
}

// Rewrites the whole word except PREV_IN_USE, which belongs to the neighbour
// on the left and survives every change made here.
static inline void mm_set_word(mm_block *b, size_t block_size, size_t slack,
                               bool used, bool quarantined) {
  uint64_t prev = b->word & MM_W_PREV_IN_USE;
  b->word = ((uint64_t)(block_size / MM_ALIGNMENT) << MM_W_SIZE_SHIFT) |
            (((uint64_t)slack & MM_W_SLACK_MASK) << MM_W_SLACK_SHIFT) |
            (quarantined ? MM_W_QUARANTINED : 0) | (used ? MM_W_IN_USE : 0) |
            prev;
}

// --- Geometry --------------------------------------------------------------

static inline uint8_t *mm_payload_of(const mm_block *b) {
  return (uint8_t *)(uintptr_t)(const void *)b + MM_HDR_SIZE;
}

static inline uint8_t *mm_block_end(const mm_block *b) {
  return (uint8_t *)(uintptr_t)(const void *)b + mm_block_size(b);
}

static inline mm_block *mm_next_block(const mm_block *b) {
  return (mm_block *)(void *)mm_block_end(b);
}

// --- Free-block interior ---------------------------------------------------
//
// A free block's payload space is unused, so the free-list links live in it
// and cost nothing. They are stored XOR-ed with the arena secret: a stray
// write that happens to look like a pointer does not survive validation, and
// a link copied from another block does not either.

static inline void *mm_free_link_get(const mm_block *b, int which,
                                     uint64_t secret) {
  uint64_t raw;
  memcpy(&raw, mm_payload_of(b) + (size_t)which * sizeof(uint64_t),
         sizeof(raw));
  uint64_t v = raw ^ secret;
  return (void *)(uintptr_t)v;
}

static inline void mm_free_link_set(mm_block *b, int which, const void *p,
                                    uint64_t secret) {
  uint64_t raw = (uint64_t)(uintptr_t)p ^ secret;
  memcpy(mm_payload_of(b) + (size_t)which * sizeof(uint64_t), &raw,
         sizeof(raw));
}

#define MM_LINK_NEXT 0
#define MM_LINK_PREV 1

// The boundary tag: a free block repeats its extent in its last eight bytes,
// so the block that follows can step backwards over it in O(1). Legible only
// when the following block's PREV_IN_USE is clear -- read at any other time
// these are payload bytes.
static inline void mm_write_free_footer(mm_block *b) {
  uint64_t n = (uint64_t)mm_block_size(b);
  memcpy(mm_block_end(b) - sizeof(n), &n, sizeof(n));
}

static inline uint64_t mm_read_prev_footer(const mm_block *b) {
  uint64_t n;
  memcpy(&n, (const uint8_t *)(const void *)b - sizeof(n), sizeof(n));
  return n;
}

// --- Checksums and canary --------------------------------------------------
//
// Both are bound to the block's position in the arena and to a per-arena
// secret drawn at mm_init. That is what turns them from bit-flip detectors
// into block-confusion detectors as well.

#if MM_HAS_CRC
static inline uint32_t mm_hdr_crc_of(const mm_block *b, uint64_t index,
                                     uint64_t secret) {
  // Fed as four 64-bit words rather than as a packed struct, so no padding
  // byte of indeterminate value ever reaches the checksum.
  uint64_t f[4];
  f[0] = b->word;
  f[1] = (uint64_t)b->payload_crc;
  f[2] = index;
  f[3] = secret;
  return mm_crc32c(f, sizeof(f));
}
#endif

static inline uint32_t mm_payload_crc(const void *payload, size_t len) {
  uint32_t c = mm_crc32c(payload, len);
  return c == 0 ? 1u : c;  // keep 0 free as "not established"
}

static inline uint64_t mm_canary_value(uint64_t index, uint64_t secret) {
  // The index is spread through the word before being mixed in, so that two
  // neighbouring blocks differ in every part of their canary rather than only
  // in the low bits.
  return MM_CANARY ^ secret ^ (index * 0x9E3779B97F4A7C15ULL);
}

#endif  // MARS_MM_LAYOUT_H_
