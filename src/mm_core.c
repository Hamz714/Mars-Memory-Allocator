// Arena setup, allocation, release, splitting and coalescing.

#include "mm_internal.h"

#include <string.h>
#include <time.h>

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

static uint64_t draw_secret(void) {
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

// --- Free list -------------------------------------------------------------

// Whether `b`'s links agree with the blocks they point at and with the head.
static bool links_consistent(const mm_block *b) {
  uint64_t s = g_arena.secret;
  const mm_block *next = (const mm_block *)mm_free_link_get(b, MM_LINK_NEXT, s);
  const mm_block *prev = (const mm_block *)mm_free_link_get(b, MM_LINK_PREV, s);

  if (next != NULL) {
    if (!mm_header_ok(next) || mm_is_used(next)) return false;
    if (mm_free_link_get(next, MM_LINK_PREV, s) != b) return false;
  }
  if (prev != NULL) {
    if (!mm_header_ok(prev) || mm_is_used(prev)) return false;
    if (mm_free_link_get(prev, MM_LINK_NEXT, s) != b) return false;
  } else if (g_arena.free_head != b) {
    return false;  // only the head has no predecessor
  }
  return true;
}

void mm_free_list_rebuild(void) {
  uint64_t s = g_arena.secret;
  g_arena.free_head = NULL;

  mm_block *tail = NULL;
  size_t budget = mm_max_blocks();
  for (uint8_t *p = g_arena.lo; p < g_arena.hi;) {
    if (budget-- == 0) break;
    mm_block *b = (mm_block *)(void *)p;
    // Without a legible header there is no way to step past this block, so the
    // rebuild stops here rather than guessing. Free blocks beyond it drop out
    // of the list; they are still in the tiling, and mm_check_heap will say so.
    if (!mm_header_ok(b)) break;

    if (!mm_is_used(b)) {
      mm_free_link_set(b, MM_LINK_PREV, tail, s);
      mm_free_link_set(b, MM_LINK_NEXT, NULL, s);
      if (tail != NULL) {
        mm_free_link_set(tail, MM_LINK_NEXT, b, s);
      } else {
        g_arena.free_head = b;
      }
      tail = b;
    }
    p += mm_block_size(b);
  }
}

void mm_free_list_insert(mm_block *b) {
  uint64_t s = g_arena.secret;
  mm_block *head = g_arena.free_head;

  if (head != NULL && (!mm_header_ok(head) || mm_is_used(head))) {
    // The list cannot be extended through a head that no longer stands up.
    // `b` is already sealed and marked free, so a rebuild picks it up.
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_free_list_rebuild();
    return;
  }

  mm_free_link_set(b, MM_LINK_NEXT, head, s);
  mm_free_link_set(b, MM_LINK_PREV, NULL, s);
  if (head != NULL) mm_free_link_set(head, MM_LINK_PREV, b, s);
  g_arena.free_head = b;
}

void mm_free_list_remove(mm_block *b) {
  uint64_t s = g_arena.secret;

  if (!links_consistent(b)) {
    // Splicing through links that do not corroborate each other is how a
    // damaged free list turns into a wild write. Rebuild from the tiling --
    // which is the authority -- and try once more.
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_free_list_rebuild();
    if (!links_consistent(b)) {
      // The rebuild could not reach `b`, so it is not on the list at all.
      mm_free_link_set(b, MM_LINK_NEXT, NULL, s);
      mm_free_link_set(b, MM_LINK_PREV, NULL, s);
      return;
    }
  }

  mm_block *next = (mm_block *)mm_free_link_get(b, MM_LINK_NEXT, s);
  mm_block *prev = (mm_block *)mm_free_link_get(b, MM_LINK_PREV, s);

  if (prev != NULL) {
    mm_free_link_set(prev, MM_LINK_NEXT, next, s);
  } else {
    g_arena.free_head = next;
  }
  if (next != NULL) mm_free_link_set(next, MM_LINK_PREV, prev, s);
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

  g_arena.base = (uint8_t *)heap;
  g_arena.size = heap_size;
  g_arena.lost_bytes = 0;
  g_arena.free_head = NULL;
  g_arena.secret = draw_secret();

  // Clear the arena once, up front. A payload checksum covers the whole
  // payload, including bytes the caller has not written yet, so those bytes
  // have to hold something defined -- otherwise the checksum is computed over
  // indeterminate memory. Paying for it here is O(arena) once rather than
  // O(size) on every allocation, and mmap-backed arenas arrive zeroed anyway.
  memset(g_arena.base, 0, heap_size);

  // The payload, not the block, is what must be 16-aligned, so a header whose
  // size is not a multiple of the alignment shifts the whole tiling forward by
  // the difference. Whatever does not divide into blocks at the tail is simply
  // not part of the arena.
  size_t usable = heap_size - MM_BLOCK_OFFSET;
  usable -= usable % MM_ALIGNMENT;
  g_arena.lo = g_arena.base + MM_BLOCK_OFFSET;
  g_arena.hi = g_arena.lo + usable;

  mm_block *first = (mm_block *)(void *)g_arena.lo;
  first->word = 0;
#if MM_HAS_CRC
  first->payload_crc = 0;
#endif
  mm_set_word(first, usable, 0, false, false);
  // Nothing precedes the first block, and reporting its predecessor as in use
  // is what stops anything trying to step backwards off the front.
  mm_set_prev_in_use(first, true);
  mm_publish(first);
  mm_free_list_insert(first);

  return 0;
}

// --- Block search ----------------------------------------------------------

// Smallest free block that fits. The walk is linear over the free list and
// stays that way in this change: binning is a separate piece of work, and
// keeping the two apart is what makes it possible to attribute a surprise in
// the numbers to one of them rather than to both at once.
static mm_block *find_best_fit(size_t need) {
  uint64_t s = g_arena.secret;

  for (int attempt = 0; attempt < 2; attempt++) {
    mm_block *best = NULL;
    size_t best_size = SIZE_MAX;
    size_t budget = mm_max_blocks();
    mm_block *back = NULL;
    bool damaged = false;

    for (mm_block *cur = g_arena.free_head; cur != NULL;) {
      if (budget-- == 0) {
        damaged = true;  // a link has gone circular; stop rather than spin
        break;
      }
      if (!mm_header_ok(cur) || mm_is_used(cur) ||
          mm_free_link_get(cur, MM_LINK_PREV, s) != back) {
        damaged = true;
        break;
      }
      size_t n = mm_block_size(cur);
      if (n >= need && n < best_size) {
        best = cur;
        best_size = n;
        if (n == need) break;  // exact fit, stop looking
      }
      back = cur;
      cur = (mm_block *)mm_free_link_get(cur, MM_LINK_NEXT, s);
    }

    if (!damaged) return best;
    mm_fail(MM_ERR_CORRUPT_LINKS);
    mm_free_list_rebuild();
  }
  return NULL;
}

// --- Splitting and coalescing ----------------------------------------------

// Merges `b` with the block after it when that one is free. `b` must itself be
// free and on the free list; it stays there, since growing it does not move
// the links inside it.
static void coalesce_forward(mm_block *b) {
  uint8_t *end = mm_block_end(b);
  if (end >= g_arena.hi) return;

  mm_block *n = (mm_block *)(void *)end;
  // A neighbour that fails validation is never merged and never written
  // through. Quarantined blocks report as in use, so they are excluded here
  // without needing a case of their own.
  if (!mm_header_ok(n) || mm_is_used(n)) return;

  mm_free_list_remove(n);
  mm_set_block_size(b, mm_block_size(b) + mm_block_size(n));
  mm_publish(b);
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
  mm_set_word(tail, leftover, 0, false, false);
  mm_set_prev_in_use(tail, true);  // `b` is in use whenever a split happens
  mm_publish(tail);
  mm_free_list_insert(tail);

  // The block that followed may itself be free -- shrinking an allocation in
  // place is the case where that happens. Leaving two free blocks side by side
  // would fragment the arena permanently.
  coalesce_forward(tail);
}

// Stamps a block as holding exactly `size` bytes for the caller, and seals it.
static void finish_allocation(mm_block *b, size_t size) {
  size_t total = mm_block_size(b);
  mm_set_word(b, total, total - MM_HDR_SIZE - size, true, false);
  mm_write_canary(b);
  mm_publish(b);
}

// --- Allocation ------------------------------------------------------------

void *mm_malloc(size_t size) {
  mm_clear_error();
  if (g_arena.base == NULL) {
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

  mm_block *b = find_best_fit(need);
  if (b == NULL) {
    MM_STAT_NOTE(MM_STAT_ALLOC_FAILED, 0);
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  mm_free_list_remove(b);
  // Marked in use at its full extent before anything is carved off it, so that
  // the arena remains walkable even in the middle of the split.
  mm_set_word(b, mm_block_size(b), 0, true, false);
  mm_seal(b);

  split_block(b, need);

#if MM_HAS_CRC
  // Deliberately left unestablished: the payload has not been written yet, so
  // there is nothing meaningful to checksum, and doing it here would make
  // every allocation cost O(size).
  b->payload_crc = 0;
#endif
  finish_allocation(b, size);

  MM_STAT_NOTE(MM_STAT_ALLOC, size);
  MM_STAT_ADD(b);
  return mm_payload_of(b);
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

  mm_set_word(b, mm_block_size(b), 0, false, false);
#if MM_HAS_CRC
  b->payload_crc = 0;
#endif
  mm_publish(b);
  mm_free_list_insert(b);

  coalesce_forward(b);

  // Backwards is the sharp edge: the predecessor's boundary tag is only
  // legible because PREV_IN_USE says so, and mm_prev_free_block corroborates
  // the tag against the header it claims to describe before returning it.
  mm_block *prev = mm_prev_free_block(b);
  if (prev != NULL) {
    mm_free_list_remove(b);
    mm_set_block_size(prev, mm_block_size(prev) + mm_block_size(b));
    mm_publish(prev);
  }
}

// --- Resize ----------------------------------------------------------------

// The payload checksum only means anything when every byte it covers has been
// written. Carrying it forward is therefore valid exactly when the surviving
// payload is entirely initialised -- that is, when the block is not growing.
// 0 marks it unestablished, a value mm_payload_crc never returns.
static void carry_payload_crc(mm_block *b, size_t old_size, uint32_t old_sum,
                              size_t new_size) {
#if MM_HAS_CRC
  if (old_sum != 0 && new_size <= old_size) {
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
    finish_allocation(b, new_size);
    MM_STAT_ADD(b);
    return payload;
  }

  // Try to absorb the following block rather than move.
  uint8_t *end = mm_block_end(b);
  if (end < g_arena.hi) {
    mm_block *n = (mm_block *)(void *)end;
    if (mm_header_ok(n) && !mm_is_used(n) &&
        mm_block_size(b) + mm_block_size(n) >= need) {
      MM_STAT_SUB(b);
      mm_free_list_remove(n);
      mm_set_block_size(b, mm_block_size(b) + mm_block_size(n));
      mm_seal(b);
      split_block(b, need);
      carry_payload_crc(b, old_size, old_sum, new_size);
      finish_allocation(b, new_size);
      MM_STAT_ADD(b);
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
