// Internals shared between the allocator's translation units. Not installed.

#ifndef MARS_MM_INTERNAL_H_
#define MARS_MM_INTERNAL_H_

#include "mars/allocator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm_freelist.h"
#include "mm_layout.h"
#include "mm_lock.h"

// --- Spans -----------------------------------------------------------------
//
// A *span* is one contiguous tiling. Blocks fill [lo, hi) exactly, always --
// there is no list to fall out of step with the tiling, because the tiling *is*
// the list. Stepping forward is `b + block_size`; stepping backward over a free
// neighbour is its boundary tag. Quarantined blocks stay in the tiling as
// permanently-allocated ones nobody owns, which is why lost space can be
// checked rather than assumed.
//
// There used to be exactly one span, and the arena struct held its bounds
// directly. Growth is what made that untenable: a caller-supplied buffer cannot
// be extended, so more memory means another region, and two regions are not one
// tiling. Everything reasoning about a block's geometry therefore asks which
// span the block is in first -- and getting that answer in O(1) for any pointer
// at all, including one this allocator never handed out, is what the chunk
// alignment in mm_arena.h exists to buy.
//
// Blocks never straddle a span. That is what keeps coalescing, the boundary
// tags and the backward walk exactly as they were: inside a span nothing about
// the geometry has changed at all.

typedef enum mm_span_kind {
  MM_SPAN_USER,   // the caller's buffer, from mm_init. Never unmapped.
  MM_SPAN_CHUNK,  // a chunk this allocator mapped and tiles ordinarily
  MM_SPAN_LARGE   // a dedicated mapping holding one oversized block
} mm_span_kind_t;

typedef struct mm_span {
  // Guards the registry itself. The descriptor for a mapped chunk lives at the
  // front of the chunk it describes, which is memory an overrun could reach;
  // XOR-ing its own address in means a descriptor copied from elsewhere fails
  // here as well as one that was overwritten.
  uint64_t magic;
  uint8_t *lo;  // first block
  uint8_t *hi;  // one past the last block
  struct mm_span *next;

  // Where this span's block indices start. A header checksum and a canary are
  // both bound to a block's index, which is what makes them detect a block
  // being confused with another one rather than only a bit being flipped
  // inside it. Indices therefore have to stay unique across spans, so every
  // span is handed a disjoint range instead of each one starting at zero.
  uint64_t index_base;

  // The mapping to hand back when this span is released, which is not the same
  // as [lo, hi): a chunk is over-mapped and trimmed to get its alignment, and
  // its descriptor sits below `lo`.
  void *map_base;
  size_t map_size;

  uint8_t kind;
  // Pages the kernel has supplied and nothing has written to since, so they
  // are still zero. What calloc uses to skip its memset.
  bool fresh;
  // The operation count from which this span may hand its pages back to the
  // operating system again. See mm_arena.h.
  uint64_t trim_after;
} mm_span;

#define MM_SPAN_MAGIC 0x5350414E5F4D4152ULL  // "SPAN_MAR", big-endian

static inline uint64_t mm_span_magic_for(const mm_span *s) {
  return MM_SPAN_MAGIC ^ (uint64_t)(uintptr_t)s;
}

// --- Arena state -----------------------------------------------------------

typedef struct mm_arena {
  // The spans, newest first. Enumerated by the whole-arena walks -- the
  // consistency check, the bin rebuild and the patrol -- and by nothing on the
  // allocation path, which reaches a span through the bins instead.
  mm_span *spans;
  size_t span_count;
  size_t total_bytes;  // sum of every span's [lo, hi)

  // The span the last lookup answered from. A managed-API arena has exactly
  // one span, so this makes mm_span_of a range check there; under the shim it
  // is a locality bet that costs one compare when it loses.
  mm_span *span_cache;

  // Handed to the next span, then advanced past that span's capacity.
  uint64_t next_index_base;

  // Heads of the size-classed free lists, and one bit per non-empty bin so
  // that the smallest sufficient one is found with a mask and a bit-scan
  // rather than by looking. See mm_freelist.h for how the sizes are cut. The
  // bins are shared by every span: a free block is filed by its size and
  // nothing else, which is what keeps allocation O(1) however many chunks
  // there are.
  mm_block *bins[MM_BIN_COUNT];
  uint64_t bin_bitmap[MM_BIN_WORDS];

  // Drawn once per arena and mixed into every checksum, canary and free-list
  // link. A corruption detector, not a security boundary.
  uint64_t secret;

  // Space belonging to quarantined blocks. Deliberately never reclaimed, but
  // tracked, so that the consistency check can tell the difference between
  // memory that was given up on purpose and memory that has gone missing.
  size_t lost_bytes;

  // Where the next patrol resumes, and in which span. Always a block boundary:
  // anything that makes it interior to a block moves it back, through
  // mm_scrub_forget.
  mm_span *scrub_span;
  uint8_t *scrub_at;

  // May the allocator map more memory when it runs out? Off for a
  // caller-supplied arena, which is fixed by definition.
  bool growable;

  // How many spans have never had a block allocated out of them, and are
  // therefore still exactly the zeroes the kernel supplied. Kept as a count so
  // that the allocation path can skip the question entirely with one load,
  // which is what it does for every allocation after the first few.
  size_t fresh_spans;

  // Monotonic count of public allocator calls, advanced by mm_scrub_tick at the
  // end of each one. The patrol has its own counter because it resets; this one
  // never does, and is what rate-limits per-span page trimming.
  uint64_t ops;

  // Whether payload integrity can be maintained at all. See mm_set_mode.
  mm_mode_t mode;

  // Allocator activity. In the arena rather than a file-static of its own,
  // because an arena is exactly the scope over which "live blocks" and "peak
  // occupancy" mean anything -- and because a counter outside the thing the
  // lock protects is a counter the lock does not protect. Only ever touched
  // with this arena's lock held.
  mm_stats_t stats;

  // Calls since the patrol last ran, for the same reason: the patrol walks one
  // arena's spans, so how overdue it is is that arena's property. The interval
  // and the budget are configuration and stay global.
  size_t ops_since_scrub;

  // Held for the whole of any operation that reads or writes this arena's
  // block metadata. See mm_lock.h -- including for why the read-only calls are
  // inside it too.
  mm_mutex lock;
} mm_arena;

extern mm_arena g_arena;

static inline bool mm_arena_live(void) { return g_arena.spans != NULL; }

// The arena's only span. For the tests and the fault injector, which reach in
// to corrupt a specific block and were written when a single contiguous
// tiling was the only thing there was. NULL when nothing is installed.
static inline mm_span *mm_sole_span(void) { return g_arena.spans; }

// --- Entering and leaving --------------------------------------------------
//
// Every public entry point is a thin wrapper: take the arena, do the work,
// give it back. The work itself is written against `g_arena` exactly as it was
// when there was no lock, which is what kept this change out of the allocation
// path altogether.
//
// **At most one arena lock is ever held at a time.** There is one arena today,
// so that is trivially true; it is written down because it is the invariant
// that has to survive there being more than one, and because it is why
// mm_realloc and mm_memalign call unlocked helpers rather than the public
// entry points they used to.

// The arena the calling thread works in, locked. Never NULL.
static inline mm_arena *mm_enter(void) {
  mm_mutex_lock(&g_arena.lock);
  return &g_arena;
}

static inline void mm_leave(mm_arena *a) { mm_mutex_unlock(&a->lock); }

// Whether a payload checksum is being maintained, and therefore whether one
// may be established or believed. False under MM_MODE_LIBC and false under a
// profile that carries no checksum at all.
bool mm_payload_crc_live(void);

// The value the next arena's secret is drawn from. Exposed so that both entry
// points -- a caller-supplied buffer and a growable arena -- draw it the same
// way, including honouring a pinned one.
uint64_t mm_draw_secret(void);

// Records the calling thread's status and returns it, so call sites can write
// `return mm_fail(MM_ERR_OOB);`.
mm_status_t mm_fail(mm_status_t status);
void mm_clear_error(void);

// --- Span lookup (mm_arena.c) ----------------------------------------------

// The span containing `p`, or NULL when none does -- which is the answer for a
// pointer this allocator never handed out, and the reason a foreign pointer is
// never dereferenced.
//
// Out of line so that the cache check below stays a compare and a branch.
mm_span *mm_span_lookup(const void *p);

static inline mm_span *mm_span_of(const void *p) {
  const uint8_t *q = (const uint8_t *)p;
  mm_span *c = g_arena.span_cache;
  if (c != NULL && q >= c->lo && q < c->hi) return c;
  return mm_span_lookup(p);
}

// Position of a block within its span, in alignment units, offset by the
// span's own base so that indices stay unique across the whole arena.
static inline uint64_t mm_index_in(const mm_span *s, const mm_block *b) {
  return s->index_base +
         (uint64_t)(((const uint8_t *)(const void *)b - s->lo) / MM_ALIGNMENT);
}

// The same for a block whose span has not already been looked up. Returns 0
// for an address in no span; every caller has range-checked first, and one
// that has not gets a number which will not check out.
static inline uint64_t mm_block_index(const mm_block *b) {
  const mm_span *s = mm_span_of(b);
  return s == NULL ? 0 : mm_index_in(s, b);
}

// --- Geometry helpers ------------------------------------------------------

static inline size_t mm_round_up(size_t n, size_t to) {
  return (n + to - 1) & ~(to - 1);
}

// Block size needed to satisfy a request of `payload` bytes, or 0 if the
// computation would overflow.
size_t mm_size_for(size_t payload);

// --- The libc-facing entry points ------------------------------------------
//
// Not in the public header. They exist for the preload shim, which is in this
// tree and is the only caller; the managed API is what the header describes,
// and widening it with three functions that only make sense underneath a
// malloc would misrepresent what this allocator is for.

// mm_malloc, plus the answer to the one question calloc has to ask: how many
// bytes at the front of the returned payload did the allocator itself write,
// and therefore cannot be assumed to be the zeroes the kernel supplied?
//
// SIZE_MAX means "assume nothing", which is the answer for every block cut out
// of memory that has been used before. Anything else means the rest of the
// payload is already zero and memset can stop early. `dirty_prefix` may be
// NULL, and the freshness is consumed either way -- it is a property of the
// moment the block was carved, not of what the caller meant to do with it.
void *mm_malloc_fresh(size_t size, size_t *dirty_prefix);

// Bytes of a fresh block's payload the allocator has written: the two
// free-list link words that were sitting in the free block it was carved from.
#define MM_FRESH_DIRTY_PREFIX (2 * sizeof(uint64_t))

// Allocates `size` bytes at an address that is a multiple of `align`, which
// must be a power of two. Alignments up to MM_ALIGNMENT are already
// guaranteed and cost nothing extra.
void *mm_memalign(size_t align, size_t size);

// Bytes the caller of `ptr` may legally use, or 0 if `ptr` is not a live block
// of ours. Under MM_MODE_LIBC that is the whole block less its trailer -- see
// finish_allocation -- which is what makes it safe to report.
size_t mm_usable_size(const void *ptr);

// True if `b` could be the *start* of a block: inside some span, on that
// span's alignment, with room after it for the smallest block there is. Says
// nothing about the extent the control word records.
//
// Separate from mm_is_block because the two halves fail for different reasons
// and want different answers. An address that is not a block start was never
// ours; a block start whose recorded extent is impossible is a block whose
// beginning is known and whose length has been lost, which is precisely the
// case recovery exists to handle.
bool mm_is_block_start(const mm_block *b);

// True if `b` could be a block header: a block start whose recorded extent is
// at least MM_MIN_BLOCK and does not run past the end of its span.
bool mm_is_block(const mm_block *b);

// Whether the tiling corroborates this block's extent, as opposed to merely
// permitting it.
//
// A free block repeats its extent in its last eight bytes. Under `hardened` and
// `paranoid` the header checksum has already vouched for the extent, so the tag
// adds nothing to what mm_header_ok established. Under `fast` there is no
// checksum: the tag is the only second copy of the extent that exists anywhere,
// and a run of payload bytes that happens to read as a free block is otherwise
// indistinguishable from a real one.
//
// Every walk that would *write into* a block on the strength of having found it
// has to ask this rather than mm_header_ok. Filing a block into a bin writes
// two link words into it; ending an abandoned run at an address writes a
// header there. Getting either wrong scatters metadata across the arena.
bool mm_extent_corroborated(const mm_block *b);

// Whether `b` is still the block it was just validated as being: same extent,
// same state.
//
// Called after every bin operation, because a bin operation writes into free
// blocks -- and one that finds its bin damaged rebuilds from the tiling, which
// writes into every free block the walk reaches. An extent read before such a
// call and used after it is not a block size; it is whatever that write left
// behind.
bool mm_unchanged(const mm_block *b, size_t block_size, bool used);


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
//
// Returns false when it refused, which happens when `b`'s recorded extent is
// not inside its span. Both writes it makes are positioned by that extent, so
// this is the last place the promise never to write outside the arena can be
// kept whatever a corrupted control word says. Refusing quietly would leave the
// tiling broken, so a refusal reports and surrenders the run; a caller that was
// about to file `b` or hand it out must not.
bool mm_publish(mm_block *b);

// Gives up on a block: it stays in the tiling so the arena still tiles, but
// it is marked quarantined, never merged and never handed out again. A block
// whose header cannot be trusted is routed through mm_rescue instead, since
// its own extent is the thing in doubt.
void mm_quarantine(mm_block *b);

// Tries to make a block with an untrustworthy header trustworthy again.
//
// Returns true when the header now stands up -- only possible where the
// profile carries a tail mirror. Otherwise the block's extent is surrendered
// as a quarantined block and false is returned. Either way the arena still
// tiles when this returns.
//
// **The caller vouches that `b` is a block start.** The extent is the thing
// being recovered, so it cannot be the evidence; what stands in for it is the
// caller knowing where the block begins -- from a pointer it handed out, or
// from the block it is itself in the middle of operating on. An address that
// merely looks plausible is not enough: giving up a run writes a control word
// at its start, and doing that at an address that is not a boundary destroys
// eight bytes of somebody's payload to tidy up a block that was never there.
bool mm_rescue(mm_block *b);

// The block preceding `b`, reached through its boundary tag, or NULL when
// there is none, when PREV_IN_USE forbids looking, or when what is there does
// not corroborate itself.
mm_block *mm_prev_free_block(const mm_block *b);

// Recovers a block header from a payload pointer, validating as it goes.
// Returns NULL and sets the thread status if the pointer is not ours.
mm_block *mm_block_of(const void *ptr);

// Whether `ptr` points into a payload this allocator handed out. Sets no
// status and reports nothing: the shim has to ask this of every pointer a
// program gives it, including ones allocated before the shim was loaded, and a
// question asked that often must not disturb what the caller last saw.
bool mm_owns(const void *ptr);

// Upper bound on how many blocks the arena can hold; bounds every traversal so
// that a corrupted link cannot produce an unbounded loop.
size_t mm_max_blocks(void);

// The same for one span, which is what the per-span walks bound themselves by.
size_t mm_span_max_blocks(const mm_span *s);

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
// [from, to). Called wherever two blocks merge or a run is surrendered, since
// those are the only things that can make a block boundary stop being one.
void mm_scrub_forget(uint8_t *from, uint8_t *to);

// Takes the patrol cursor off a span that is about to stop existing.
void mm_scrub_leave(const mm_span *s);

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
// identifies the damaged block exactly and it is rebuilt; otherwise its extent
// is surrendered as a quarantined block and counted in lost_bytes.
//
// Returns NULL when nothing follows it in its span. Never returns a block it
// has not made consistent with `owner`.
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
