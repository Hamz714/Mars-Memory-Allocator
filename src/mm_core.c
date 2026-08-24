// Arena setup, allocation, release, splitting and coalescing.

#include "mm_internal.h"

#include <string.h>
#include <time.h>

#include "mm_arena.h"

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

  // Whatever was installed before goes back to the operating system here, so
  // that a program which starts with a growable arena and then hands over a
  // buffer of its own -- which the tests do, repeatedly -- leaks nothing.
  mm_arena_reset();
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
    mm_fail(MM_ERR_BAD_ARENA);
    return -1;
  }
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

  if (mm_block_size(b) >= MM_DONTNEED_MIN) {
    // Only the interior. The first two words of a free block's payload are its
    // free-list links and the last eight bytes are its boundary tag, and
    // handing either back would be handing back the block's own metadata.
    mm_arena_release_pages(mm_payload_of(b) + 2 * sizeof(uint64_t),
                           mm_block_end(b) - sizeof(uint64_t));
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

// Stamps a block as holding exactly `size` bytes for the caller, and seals it.
// False when the block could not be published, in which case there is nothing
// to hand back to the caller.
static bool finish_allocation(mm_block *b, size_t size) {
  size_t total = mm_block_size(b);
  mm_set_word(b, total, total - MM_HDR_SIZE - size, true, false);
  mm_write_canary(b);
  return mm_publish(b);
}

// --- Allocation ------------------------------------------------------------

// Maps more memory when the bins have nothing that fits, and returns a free
// block from it. NULL when the arena is fixed, or when the mapping failed.
//
// The new span arrives with its whole space filed as one free block, so the
// answer comes back out of the bins rather than being handed over directly.
// That is not a detour: it keeps every allocation, grown-into or not, on
// exactly one path through mm_bin_remove and the re-validation after it.
static mm_block *grow_for(size_t need) {
  if (!g_arena.growable) return NULL;
  mm_span *s = need > MM_LARGE_THRESHOLD ? mm_arena_map_large(need)
                                         : mm_arena_grow(need);
  if (s == NULL) return NULL;
  return mm_bin_find(need);
}

void *mm_malloc(size_t size) {
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

  mm_block *b = mm_bin_find(need);
  if (b == NULL) b = grow_for(need);
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
  mm_clear_error();
  if (ptr == NULL) return;

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
  mm_clear_error();
  if (ptr == NULL) return mm_malloc(new_size);

  if (new_size == 0) {
    mm_free(ptr);
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
  void *fresh = mm_malloc(new_size);
  if (fresh == NULL) return NULL;

  size_t carry = old_size < new_size ? old_size : new_size;
  memcpy(fresh, payload, carry);

  mm_block *fresh_block = mm_block_of(fresh);
  if (fresh_block != NULL) {
    carry_payload_crc(fresh_block, carry, old_sum, new_size);
    mm_seal(fresh_block);
  }

  mm_free(ptr);
  return fresh;
}
