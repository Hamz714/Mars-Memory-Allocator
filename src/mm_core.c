// Arena setup, allocation, release, splitting and coalescing.

#include "mm_internal.h"

#include <string.h>
#include <time.h>

#include "mm_arena.h"

// --- The unlocked forms ----------------------------------------------------
//
// Every public entry point below is a wrapper: mm_enter, the work, mm_leave.
// The work is these, and they exist because realloc and memalign are written
// in terms of malloc and free -- calling the public forms from inside the lock
// would take it twice, and a mutex that is not recursive is exactly the right
// kind for an allocator to have. Making the nesting impossible is better than
// making it survivable.
static void *malloc_unlocked(size_t size, size_t *dirty_prefix);
static void free_unlocked(void *ptr);
static size_t size_of_live(const void *ptr);
static void *memalign_unlocked(size_t align, size_t size);
static void *realloc_unlocked(void *ptr, size_t new_size);
#if MM_LOCK == MM_LOCK_ARENA
static void *realloc_foreign(void *ptr, size_t new_size);
#endif

size_t mm_alignment(void) { return MM_ALIGNMENT; }

size_t mm_min_arena(void) { return MM_BLOCK_OFFSET + MM_MIN_BLOCK; }

// Block size required to hold `payload` bytes, or 0 if that would overflow.
size_t mm_size_for(size_t payload) {
  if (payload > SIZE_MAX - MM_HDR_SIZE - MM_TRAIL - MM_ALIGNMENT) return 0;
  size_t need = mm_round_up(payload + MM_HDR_SIZE + MM_TRAIL, MM_ALIGNMENT);
  return need < MM_MIN_BLOCK ? MM_MIN_BLOCK : need;
}

// --- The arena secret ------------------------------------------------------

static _Thread_local uint64_t g_pinned_secret;

void mm_pin_secret(uint64_t secret) { g_pinned_secret = secret; }

uint64_t mm_draw_secret(void) {
  if (g_pinned_secret != 0) return g_pinned_secret;

  // Address of a stack object and the clock, run through splitmix64. Under
  // ASLR the first is the real source of entropy and the others keep two
  // arenas in the same process apart. This is a corruption detector, not a
  // security boundary, and it is not claimed to be one.
  uint64_t z = (uint64_t)(uintptr_t)&z;
  z ^= (uint64_t)time(NULL) << 20;
  z ^= (uint64_t)clock() * 0x2545F4914F6CDD1DULL;

  z += 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return z | 1u;  // never 0: that value means "pin nothing"
}

// --- Initialisation --------------------------------------------------------

// Installing an arena is a lifecycle call, not an allocator call: it discards
// whatever was there, which is not something that can be made safe against a
// thread allocating out of it at the same time. The lock is still taken, so
// that a program which happens to serialise its own start-up correctly is not
// then betrayed by a half-published arena.
int mm_init(void *heap, size_t heap_size) {
  mm_clear_error();
  if (heap == NULL || heap_size < mm_min_arena()) {
    mm_fail(MM_ERR_BAD_ARENA);
    return -1;
  }
  if ((uintptr_t)heap % MM_ALIGNMENT != 0) {
    mm_fail(MM_ERR_BAD_ARENA);
    return -1;
  }

  // Every arena there is, not only this thread's: installing a buffer discards
  // whatever was there, and leaving another thread's arena pointing at memory
  // this call is about to disown would be worse than refusing outright.
  mm_arenas_reset();
  mm_arena *a = mm_enter();

  // Whatever was installed before went back to the operating system in
  // mm_arenas_reset above, so that a program which starts with a growable arena
  // and then hands over a buffer of its own -- which the tests do, repeatedly
  // -- leaks nothing.
  g_arena.secret = mm_draw_secret();
  // A caller-supplied arena is reached through mm_read and mm_write, which is
  // what the managed mode means. Restoring it here means a program that turned
  // the shim's mode on and then installed its own arena gets the arena it
  // asked for rather than the one the shim left behind.
  g_arena.mode = MM_MODE_MANAGED;

  // Clear the arena once, up front. A payload checksum covers the whole
  // payload, including bytes the caller has not written yet, so those bytes
  // have to hold something defined -- otherwise the checksum is computed over
  // indeterminate memory. Paying for it here is O(arena) once rather than
  // O(size) on every allocation, and mmap-backed arenas arrive zeroed anyway.
  memset(heap, 0, heap_size);

  // The payload, not the block, is what must be 16-aligned, so a header whose
  // size is not a multiple of the alignment shifts the whole tiling forward by
  // the difference. Whatever does not divide into blocks at the tail is simply
  // not part of the arena. mm_arena_install_user does that arithmetic and
  // files the space as one free block.
  if (mm_arena_install_user(heap, heap_size) == NULL) {
    mm_leave(a);
    mm_fail(MM_ERR_BAD_ARENA);
    return -1;
  }
  mm_leave(a);
  return 0;
}

// --- Splitting and coalescing ----------------------------------------------
//
// A block's size decides which bin it belongs in, so merging is where bin
// membership is easiest to get wrong. The rule that makes it safe is not
// obvious and is worth stating plainly:
//
//   **a block that is free in the tiling must be in a bin.**
//
// Not "usually", but at every point where a bin operation could find damaged
// links -- because the answer to damaged links is to rebuild the bins from the
// tiling, and the rebuild files every free block it walks over. A block held
// deliberately out of the bins while it is being merged would be refiled by
// that rebuild and then merged away underneath it, leaving a bin pointing into
// the middle of somebody else's block. It costs one allocation with a damaged
// free list to reach, and the arena never recovers.
//
// So coalescing happens while the block doing the absorbing is still marked
// **in use**, which is exactly the trick mm_malloc already uses to keep the
// arena walkable through a split. Every mm_bin_remove below is reached with
// the bins and the tiling in complete agreement, so a rebuild at that moment
// reproduces what is already there and the retry succeeds. Between a removal
// and the tiling change that makes it true, nothing that could rebuild is
// called.

// Extends `b` over whichever of its neighbours are free, taking each of them
// out of its bin. `b` must be marked in use, and therefore in no bin; it stays
// that way. Returns the block now spanning the whole run, which is `b` itself
// or its predecessor, still marked in use and still in no bin.
//
// Returns NULL when `b` itself stopped standing up part-way through. Taking a
// neighbour out of a bin writes memory, and a bin found damaged is rebuilt from
// the tiling, which writes into every free block the walk reaches -- so an
// extent read before a bin operation and added to another one after it is not a
// block size but whatever that write left behind. That is not a theoretical
// ordering worry: it is the mechanism by which a two-bit flip under `fast`
// reached mm_write_free_footer with an extent 400 times the arena. Both extents
// are therefore re-established after every bin operation, and a block that no
// longer agrees with itself is given up on rather than merged.
static mm_block *absorb_neighbours(mm_block *b) {
  size_t own = mm_block_size(b);

  const mm_span *sp = mm_span_of(b);
  if (sp == NULL) return NULL;

  uint8_t *end = mm_block_end(b);
  // Bounded by the span and not the arena: the block after the last one in a
  // chunk is not a block at all, it is whatever the next mapping holds.
  if (end < sp->hi) {
    mm_block *n = (mm_block *)(void *)end;
    // A neighbour that fails validation is never merged and never written
    // through. Quarantined blocks report as in use, so they are excluded here
    // without needing a case of their own.
    if (mm_header_ok(n) && !mm_is_used(n)) {
      size_t theirs = mm_block_size(n);
      mm_bin_remove(n);
      if (!mm_unchanged(b, own, true)) {
        // `b` is the block being freed and it has stopped being itself. Nothing
        // may be written through it, so it is surrendered and the free ends
        // here. The status is set after the quarantine, not before: a damaged
        // header is why this happened, and it is more use to the caller than
        // the quarantine that followed from it.
        mm_quarantine(b);
        mm_fail(MM_ERR_CORRUPT_HEADER);
        return NULL;
      }
      if (!mm_unchanged(n, theirs, false)) {
        // The neighbour is out of its bin and no longer describes itself, so it
        // is neither mergeable nor safe to leave free in the tiling. Give it up
        // and release `b` at its own extent below.
        mm_quarantine(n);
        mm_fail(MM_ERR_CORRUPT_HEADER);
      } else {
        // Immediately, with nothing that could rebuild in between: after this
        // line `n` is not in the tiling either, and the two agree again.
        own += theirs;
        mm_set_block_size(b, own);
        // `n` has stopped being a block boundary, so a patrol cursor parked on
        // it has to come back to one.
        mm_scrub_forget((uint8_t *)(void *)b, mm_block_end(b));
        mm_seal(b);
      }
    }
  }

  // Backwards is the sharp edge: the predecessor's boundary tag is only
  // legible because PREV_IN_USE says so, and mm_prev_free_block corroborates
  // the tag against the header it claims to describe before returning it.
  mm_block *prev = mm_prev_free_block(b);
  if (prev == NULL) return b;

  size_t before = mm_block_size(prev);
  mm_bin_remove(prev);
  if (!mm_unchanged(b, own, true)) {
    mm_quarantine(b);
    mm_fail(MM_ERR_CORRUPT_HEADER);
    return NULL;
  }
  if (!mm_unchanged(prev, before, false)) {
    mm_quarantine(prev);
    mm_fail(MM_ERR_CORRUPT_HEADER);
    return b;
  }

  // In use rather than free, for the same reason as above -- and carrying the
  // trailer's worth of slack, so that the notional payload of this temporary
  // block ends inside it rather than at the arena's edge.
  mm_set_word(prev, before + own, MM_TRAIL, true, false);
  mm_scrub_forget((uint8_t *)(void *)prev, mm_block_end(prev));
  mm_seal(prev);
  return prev;
}

// Hands memory back to the operating system after a block has been freed.
//
// Two different things, because a chunk and a dedicated mapping are used
// differently. A mapping made for one oversized request has nothing else in it
// once that request is released, so it is unmapped outright -- address space
// and all. An ordinary chunk is kept and reused, because churning mappings
// would trade a free-list operation for two syscalls and a storm of page
// faults; what it gives back instead is the resident pages of a large enough
// free run, which is the memory that actually costs anything.
static void reclaim(mm_block *b) {
  mm_span *s = mm_span_of(b);
  // A caller-supplied buffer is not ours to hand anywhere. It may be static
  // storage or a file mapping, and MADV_DONTNEED means something quite
  // different on one of those.
  if (s == NULL || s->kind == MM_SPAN_USER) return;
  // The release did not end with a free block -- it was quarantined part-way
  // through -- so there is nothing here to give back.
  if (mm_is_used(b)) return;

  if (s->kind == MM_SPAN_LARGE && (uint8_t *)(void *)b == s->lo &&
      mm_block_end(b) == s->hi) {
    // Out of its bin first: the memory the links live in is about to stop
    // existing.
    mm_bin_remove(b);
    mm_arena_release_span(s);
    return;
  }

  // Large enough to be worth a syscall, and not more often than once per
  // MM_TRIM_INTERVAL calls for this span. Both conditions are load-bearing and
  // the second one was learnt from a measurement -- see mm_arena.h.
  if (mm_block_size(b) >= MM_DONTNEED_MIN && g_arena.ops >= s->trim_after) {
    // Only the interior. The first two words of a free block's payload are its
    // free-list links and the last eight bytes are its boundary tag, and
    // handing either back would be handing back the block's own metadata.
    mm_arena_release_pages(mm_payload_of(b) + 2 * sizeof(uint64_t),
                           mm_block_end(b) - sizeof(uint64_t));
    s->trim_after = g_arena.ops + MM_TRIM_INTERVAL;
  }
}

// Marks a block that absorb_neighbours produced as free, publishes it, and
// files it. The insert is the last thing to happen, and by then the extent has
// stopped moving.
//
// A block mm_publish refused is not filed: its span has already been
// surrendered, and putting a quarantined block into a bin would hand it out
// again.
static void release_block(mm_block *b) {
  mm_set_word(b, mm_block_size(b), 0, false, false);
#if MM_HAS_CRC
  b->payload_crc = 0;
#endif
  if (!mm_publish(b)) return;
  mm_bin_insert(b);
}

// Trims `b` to `need`, handing the remainder back as a free block. `b` must
// already be marked in use and off the free list; it is left sealed at its new
// extent, and the caller finishes setting its slack.
static void split_block(mm_block *b, size_t need) {
  size_t leftover = mm_block_size(b) - need;
  if (leftover < MM_MIN_BLOCK) return;

  mm_set_block_size(b, need);
  mm_seal(b);  // keep the tiling walkable at every point in between

  mm_block *tail = (mm_block *)(void *)((uint8_t *)(void *)b + need);
  tail->word = 0;
#if MM_HAS_CRC
  tail->payload_crc = 0;
#endif
  // Born in use, like every block that is about to absorb a neighbour.
  mm_set_word(tail, leftover, MM_TRAIL, true, false);
  mm_set_prev_in_use(tail, true);  // `b` is in use whenever a split happens
  mm_seal(tail);

  // The block that followed may itself be free -- shrinking an allocation in
  // place is the case where that happens. Leaving two free blocks side by side
  // would fragment the arena permanently. Nothing precedes `tail` but `b`,
  // which is in use, so the backward half of this finds nothing.
  mm_block *run = absorb_neighbours(tail);
  if (run != NULL) release_block(run);
}

// Stamps a block as holding `size` bytes for the caller, and seals it. False
// when the block could not be published, in which case there is nothing to
// hand back to the caller.
//
// Under MM_MODE_LIBC the block records the whole of itself, less its trailer,
// as payload -- not the size that was asked for. That is what makes
// malloc_usable_size answerable, and it is the answer to a hazard rather than
// a convenience. A libc program is entitled to write into every byte
// malloc_usable_size reports; if the payload ended at the requested size, the
// canary would sit inside that entitlement and a correct program overrunning
// into its own slack would be reported as corruption. Ending the payload where
// the trailer begins puts the canary back out of reach, where it can only be
// reached by an overrun that really is one.
static bool finish_allocation(mm_block *b, size_t size) {
  size_t total = mm_block_size(b);
  size_t give =
      g_arena.mode == MM_MODE_LIBC ? total - MM_HDR_SIZE - MM_TRAIL : size;
  mm_set_word(b, total, total - MM_HDR_SIZE - give, true, false);
  mm_write_canary(b);
  return mm_publish(b);
}

// --- Allocation ------------------------------------------------------------

// Maps more memory and returns a free block from it, still in its bin. NULL
// when the arena is fixed, or when the mapping failed.
//
// The block is taken from the new span's start rather than by searching the
// bins again. Searching would sometimes prefer free space elsewhere and leave
// the mapping that was just made standing empty, with nothing in it whose
// release could ever hand it back. There is nothing else in this span to
// prefer, so its first block is the answer by construction.
static mm_block *grow_for(size_t need) {
  if (!g_arena.growable) return NULL;
  mm_span *s = need > MM_LARGE_THRESHOLD ? mm_arena_map_large(need)
                                         : mm_arena_grow(need);
  if (s == NULL) return NULL;

  mm_block *b = (mm_block *)(void *)s->lo;
  if (!mm_header_ok(b) || mm_is_used(b) || mm_block_size(b) < need) {
    return NULL;
  }
  return b;
}

void *mm_malloc(size_t size) { return mm_malloc_fresh(size, NULL); }

void *mm_malloc_fresh(size_t size, size_t *dirty_prefix) {
  mm_arena *a = mm_enter();
  void *p = malloc_unlocked(size, dirty_prefix);
  mm_leave(a);
  return p;
}

static void *malloc_unlocked(size_t size, size_t *dirty_prefix) {
  if (dirty_prefix != NULL) *dirty_prefix = SIZE_MAX;
  mm_clear_error();
  if (!mm_arena_live()) {
    mm_fail(MM_ERR_NOT_INITIALIZED);
    return NULL;
  }
  if (size == 0) {
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  size_t need = mm_size_for(size);
  if (need == 0) {
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  // Above half a chunk an allocation is given a mapping of its own, and the
  // bins are not consulted first. That is not a performance choice: it is what
  // makes releasing it hand the memory back to the operating system rather
  // than only to the free lists. A multi-megabyte buffer that happened to fit
  // in an existing chunk would pin that chunk for the life of the process.
  //
  // A fixed arena is exempt, because it has nothing to map: there the bins are
  // all there is, and refusing to look in them would fail an allocation the
  // arena could serve.
  bool dedicated = g_arena.growable && need > MM_LARGE_THRESHOLD;

  mm_block *b = dedicated ? NULL : mm_bin_find(need);
  if (b == NULL) b = grow_for(need);
  // The mapping was refused. Whatever the bins hold is better than failing, so
  // the preference for a dedicated mapping gives way rather than the request.
  if (b == NULL && dedicated) b = mm_bin_find(need);
  if (b == NULL) {
    MM_STAT_NOTE(MM_STAT_ALLOC_FAILED, 0);
    mm_fail(MM_ERR_NOMEM);
    mm_scrub_tick();
    return NULL;
  }

  size_t found = mm_block_size(b);
  mm_bin_remove(b);
  // Taking the block out of its bin wrote memory, so its extent has to be
  // re-established before it is trusted again -- and it has to be, because
  // split_block subtracts `need` from it. See mm_unchanged.
  if (!mm_unchanged(b, found, false)) {
    mm_quarantine(b);
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_scrub_tick();
    return NULL;
  }
  // A block carved out of a mapping nothing has been allocated from yet is
  // still the zeroes the kernel supplied, and calloc is entitled to know.
  // Asked here rather than in calloc, and asked whether or not anybody wanted
  // the answer, because freshness is a property of the moment the block is
  // carved: once it has been handed out the allocator cannot say what the
  // caller did with it. The count makes this one load in the ordinary case.
  if (g_arena.fresh_spans != 0) {
    size_t dirty = mm_span_take_fresh(b);
    if (dirty_prefix != NULL) *dirty_prefix = dirty;
  }

  // Marked in use at its full extent before anything is carved off it, so that
  // the arena remains walkable even in the middle of the split.
  mm_set_word(b, found, 0, true, false);
  mm_seal(b);

  split_block(b, need);

#if MM_HAS_CRC
  // Deliberately left unestablished: the payload has not been written yet, so
  // there is nothing meaningful to checksum, and doing it here would make
  // every allocation cost O(size).
  b->payload_crc = 0;
#endif
  if (!finish_allocation(b, size)) {
    mm_scrub_tick();
    return NULL;
  }

  MM_STAT_NOTE(MM_STAT_ALLOC, size);
  MM_STAT_ADD(b);

  uint8_t *payload = mm_payload_of(b);
  mm_scrub_tick();
  return payload;
}

// --- Aligned allocation ----------------------------------------------------

size_t mm_usable_size(const void *ptr) {
  mm_clear_error();
  mm_guard g = mm_enter_for(ptr);
  size_t n = size_of_live(ptr);
  mm_leave_for(g);
  return n;
}

// The caller holds the lock of whichever arena owns `ptr`, and `g_arena` names
// it. 0 for anything that is not a live block of ours.
static size_t size_of_live(const void *ptr) {
  mm_block *b = mm_block_of(ptr);
  if (b == NULL || !mm_header_ok(b)) return 0;
  if (!mm_is_used(b) || mm_is_quarantined(b)) return 0;
  return mm_requested_size(b);
}

// Allocates at an address that is a multiple of `align`.
//
// The way this is usually done in a preload allocator is to over-allocate, put
// a header immediately below the aligned address, and set a flag in it saying
// how far the block was shifted so that free can find the real start again.
// That costs a bit in the control word, and this control word has none to
// spare: every field in it is load-bearing and the whole thing is checksummed.
//
// It is also unnecessary here. Blocks tile their span exactly, so the aligned
// address can be made into a genuine block start instead of a shifted one: cut
// the over-allocated block in two at that point, hand the back half out, and
// release the front half as ordinary free space. What comes back is a block
// like any other -- free, realloc, the consistency check and coalescing all
// treat it as one, with no case of their own and no flag to get wrong.
void *mm_memalign(size_t align, size_t size) {
  mm_arena *a = mm_enter();
  void *p = memalign_unlocked(align, size);
  mm_leave(a);
  return p;
}

static void *memalign_unlocked(size_t align, size_t size) {
  if (align <= MM_ALIGNMENT) return malloc_unlocked(size, NULL);

  // Enough slack that an aligned address with room for a whole block in front
  // of it is guaranteed to exist: one alignment step to reach the boundary,
  // and another in case the first one lands too close to the front to leave a
  // block there.
  size_t pad = 2 * align + MM_MIN_BLOCK;
  if (size > SIZE_MAX - pad) {
    mm_clear_error();
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  uint8_t *raw = (uint8_t *)malloc_unlocked(size + pad, NULL);
  if (raw == NULL) return NULL;

  uint8_t *want = (uint8_t *)mm_round_up((uintptr_t)raw, align);
  // The front piece has to be able to stand on its own as a block.
  while ((size_t)(want - raw) < MM_MIN_BLOCK) want += align;

  mm_block *b = mm_block_of(raw);
  if (b == NULL) return raw;  // reported already; nothing left to do safely

  size_t total = mm_block_size(b);
  size_t front = (size_t)(want - raw);
  size_t need = mm_size_for(size);
  if (need == 0 || front + need > total) {
    // Cannot be cut where it needs cutting. The block is correct and correctly
    // aligned for anything up to MM_ALIGNMENT, so it is released rather than
    // returned: handing back an under-aligned pointer would be worse than
    // failing.
    free_unlocked(raw);
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  MM_STAT_SUB(b);

  // Shrink the front, keeping its flags -- PREV_IN_USE belongs to whatever is
  // before it and IN_USE keeps the arena walkable through the cut.
  mm_set_block_size(b, front);
  mm_seal(b);

  mm_block *tail = (mm_block *)(void *)(want - MM_HDR_SIZE);
  tail->word = 0;
#if MM_HAS_CRC
  tail->payload_crc = 0;
#endif
  // Born in use, at the whole of what is left, like every block that is about
  // to be trimmed.
  mm_set_word(tail, total - front, MM_TRAIL, true, false);
  mm_set_prev_in_use(tail, true);
  mm_seal(tail);

  split_block(tail, need);
  if (!finish_allocation(tail, size)) {
    mm_scrub_tick();
    return NULL;
  }
  MM_STAT_ADD(tail);

  // Only now is the front given away. It is marked in use and is in no bin,
  // which is exactly the state absorb_neighbours expects; the block after it
  // is `tail`, which is in use, so nothing merges forward over the allocation
  // that was just made.
  mm_block *run = absorb_neighbours(b);
  if (run != NULL) {
    release_block(run);
    reclaim(run);
  }

  mm_scrub_tick();
  return mm_payload_of(tail);
}

// --- Release ---------------------------------------------------------------

// Validates a block reached from a caller's pointer and reports why not.
// Returns NULL with the thread status set when the block must not be used.
static mm_block *live_block(const void *ptr) {
  mm_block *b = mm_block_of(ptr);
  if (b == NULL) return NULL;

  if (!mm_header_ok(b)) {
    // Under a profile with a tail mirror this may put the header back; under
    // the others the block's span is surrendered. Either way the arena still
    // tiles afterwards.
    if (!mm_rescue(b)) return NULL;
  }
  if (mm_is_quarantined(b)) {
    mm_fail(MM_ERR_QUARANTINED);
    return NULL;
  }
  return b;
}

void mm_free(void *ptr) {
  // Checked before the lock is taken. free(NULL) is legal, common, and does
  // nothing; making it wait on a mutex would be a cost paid by every program
  // that frees a null pointer in a loop.
  if (ptr == NULL) {
    mm_clear_error();
    return;
  }

#if MM_LOCK == MM_LOCK_ARENA
  // The one path that must not take the owner's lock. A producer/consumer
  // program does this to every object it allocates, and waiting on the
  // producer's lock here would serialise exactly what per-thread arenas exist
  // to unserialise. See the remote-free queue in mm_internal.h -- including for
  // why nothing about the block is read or written before it goes on there.
  //
  // A full queue falls through to the lock, which is correct, slower, and the
  // back-pressure a full queue should produce.
  mm_arena *owner = mm_owner_of(ptr);
  if (owner != NULL && owner != mm_self) {
    mm_clear_error();
    if (mm_remote_push(owner, ptr)) return;
    mm_mutex_lock(&owner->lock);
    mm_arena *saved = mm_self;
    mm_self = owner;
    mm_remote_drain(owner);
    free_unlocked(ptr);
    mm_self = saved;
    mm_mutex_unlock(&owner->lock);
    return;
  }
#endif

  mm_arena *a = mm_enter();
  free_unlocked(ptr);
  mm_leave(a);
}

// Blocks other threads handed over, put through exactly the free they would
// have had if their own thread had done it -- rejections, reports and
// quarantine included. The caller holds this arena's lock.
void mm_remote_drain(mm_arena *a) {
#if MM_LOCK == MM_LOCK_ARENA
  // What the calling thread was in the middle of finding out. A queued free
  // that runs into damage still quarantines the block and still counts it; what
  // it must not do is overwrite the status of the call that happened to be
  // passing through.
  mm_status_t saved = mm_last_error();
  for (void *p = mm_remote_pop(a); p != NULL; p = mm_remote_pop(a)) {
    free_unlocked(p);
  }
  (void)mm_fail(saved);
#else
  (void)a;
#endif
}

static void free_unlocked(void *ptr) {
  mm_clear_error();

  mm_block *b = live_block(ptr);
  if (b == NULL) return;

  if (!mm_is_used(b)) {
    mm_fail(MM_ERR_DOUBLE_FREE);
    return;
  }
  if (!mm_canary_ok(b)) {
    mm_quarantine(b);
    return;
  }

  MM_STAT_NOTE(MM_STAT_FREE, mm_requested_size(b));
  MM_STAT_SUB(b);

#ifdef MM_ZERO_ON_FREE
  // Scrub the payload before the free-list links go into it, so freed contents
  // cannot leak into a later allocation.
  memset(mm_payload_of(b), 0, mm_block_size(b) - MM_HDR_SIZE);
#endif

  // Coalesce first, while `b` is still marked in use, then mark the whole run
  // free in one step. See absorb_neighbours for why that order is the safe one
  // rather than merely the tidy one.
  mm_block *run = absorb_neighbours(b);
  // NULL means the block stopped standing up mid-coalesce and has been reported
  // and surrendered. There is nothing left to release, and the caller's pointer
  // is gone either way.
  if (run != NULL) {
    release_block(run);
    reclaim(run);
  }

  mm_scrub_tick();
}

// --- Resize ----------------------------------------------------------------

#if MM_LOCK == MM_LOCK_ARENA
// A resize of a block belonging to another thread's arena. See mm_realloc.
static void *realloc_foreign(void *ptr, size_t new_size) {
  mm_clear_error();

  mm_guard g = mm_enter_for(ptr);
  size_t old_size = size_of_live(ptr);
  mm_status_t why = mm_last_error();
  mm_leave_for(g);

  if (old_size == 0) {
    mm_fail(why != MM_OK ? why : MM_ERR_INVALID_PTR);
    return NULL;
  }

  void *fresh = mm_malloc(new_size);
  if (fresh == NULL) return NULL;

  // The payload is the caller's own memory and this thread is the one holding
  // it, so copying it needs nobody's lock. It is the *metadata* that belongs to
  // the other arena, and none of it is touched here.
  memcpy(fresh, ptr, old_size < new_size ? old_size : new_size);
  mm_free(ptr);
  return fresh;
}
#endif


// The payload checksum only means anything when every byte it covers has been
// written. Carrying it forward is therefore valid exactly when the surviving
// payload is entirely initialised -- that is, when the block is not growing.
// 0 marks it unestablished, a value mm_payload_crc never returns.
static void carry_payload_crc(mm_block *b, size_t old_size, uint32_t old_sum,
                              size_t new_size) {
#if MM_HAS_CRC
  if (mm_payload_crc_live() && old_sum != 0 && new_size <= old_size) {
    b->payload_crc = mm_payload_crc(mm_payload_of(b), new_size);
  } else {
    b->payload_crc = 0;
  }
#else
  (void)b;
  (void)old_size;
  (void)old_sum;
  (void)new_size;
#endif
}

static uint32_t payload_crc_of(const mm_block *b) {
#if MM_HAS_CRC
  return b->payload_crc;
#else
  (void)b;
  return 0;
#endif
}

void *mm_realloc(void *ptr, size_t new_size) {
#if MM_LOCK == MM_LOCK_ARENA
  // Resizing another thread's block cannot be done in place: growing or
  // shrinking it means moving its neighbours' metadata about, and this thread
  // would have to hold the owner's lock while allocating, which is the one
  // thing the deadlock argument forbids. So it becomes allocate, copy, hand
  // back -- and the hand-back is an ordinary cross-thread free.
  //
  // The old size is read under the owner's lock and the lock is then given up.
  // Nothing can change it in between: the block is live and this thread is the
  // one holding it.
  if (ptr != NULL && new_size != 0) {
    mm_arena *owner = mm_owner_of(ptr);
    if (owner != NULL && owner != mm_self) return realloc_foreign(ptr, new_size);
  }
#endif
  mm_arena *a = mm_enter();
  void *p = realloc_unlocked(ptr, new_size);
  mm_leave(a);
  return p;
}

static void *realloc_unlocked(void *ptr, size_t new_size) {
  mm_clear_error();
  if (ptr == NULL) return malloc_unlocked(new_size, NULL);

  if (new_size == 0) {
    free_unlocked(ptr);
    return NULL;
  }

  mm_block *b = live_block(ptr);
  if (b == NULL) return NULL;

  if (!mm_is_used(b)) {
    mm_fail(MM_ERR_DOUBLE_FREE);
    return NULL;
  }
  if (!mm_canary_ok(b)) {
    mm_quarantine(b);
    return NULL;
  }

  size_t need = mm_size_for(new_size);
  if (need == 0) {
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  MM_STAT_NOTE(MM_STAT_REALLOC, new_size);

  size_t old_size = mm_requested_size(b);
  uint32_t old_sum = payload_crc_of(b);
  uint8_t *payload = mm_payload_of(b);

  // Already big enough: shrink in place, handing back any surplus.
  if (mm_block_size(b) >= need) {
    MM_STAT_SUB(b);
    split_block(b, need);
    carry_payload_crc(b, old_size, old_sum, new_size);
    if (!finish_allocation(b, new_size)) {
      mm_scrub_tick();
      return NULL;
    }
    MM_STAT_ADD(b);
    mm_scrub_tick();
    return payload;
  }

  // Try to absorb the following block rather than move. Bounded by this
  // block's own span: what follows the last block of a chunk is another
  // mapping, not a block.
  const mm_span *sp = mm_span_of(b);
  uint8_t *end = mm_block_end(b);
  if (sp != NULL && end < sp->hi) {
    mm_block *n = (mm_block *)(void *)end;
    size_t own = mm_block_size(b);
    bool mergeable = mm_header_ok(n) && !mm_is_used(n);
    size_t theirs = mergeable ? mm_block_size(n) : 0;
    if (mergeable && own + theirs >= need) {
      MM_STAT_SUB(b);
      mm_bin_remove(n);
      // Both extents are re-established after the bin operation, for the reason
      // absorb_neighbours spells out: a bin operation writes into free blocks,
      // so neither number survived it vouched for.
      if (!mm_unchanged(b, own, true)) {
        mm_quarantine(b);
        mm_fail(MM_ERR_CORRUPT_HEADER);
        return NULL;
      }
      if (!mm_unchanged(n, theirs, false)) {
        MM_STAT_ADD(b);  // `b` is untouched; only the neighbour was given up
        mm_quarantine(n);
        mm_fail(MM_ERR_CORRUPT_HEADER);
        return NULL;
      }
      mm_set_block_size(b, own + theirs);
      mm_scrub_forget((uint8_t *)(void *)b, mm_block_end(b));
      mm_seal(b);
      split_block(b, need);
      carry_payload_crc(b, old_size, old_sum, new_size);
      if (!finish_allocation(b, new_size)) {
        mm_scrub_tick();
        return NULL;
      }
      MM_STAT_ADD(b);
      mm_scrub_tick();
      return payload;
    }
  }

  // Fall back to allocate, copy, release. Copy before freeing so a failed
  // allocation leaves the original block untouched.
  void *fresh = malloc_unlocked(new_size, NULL);
  if (fresh == NULL) return NULL;

  size_t carry = old_size < new_size ? old_size : new_size;
  memcpy(fresh, payload, carry);

  mm_block *fresh_block = mm_block_of(fresh);
  if (fresh_block != NULL) {
    carry_payload_crc(fresh_block, carry, old_sum, new_size);
    mm_seal(fresh_block);
  }

  free_unlocked(ptr);
  return fresh;
}
