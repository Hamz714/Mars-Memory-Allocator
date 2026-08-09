// Arena setup, allocation, release, splitting and coalescing.

#include "mm_internal.h"

#include <string.h>

size_t mm_alignment(void) { return MM_ALIGNMENT; }

size_t mm_min_arena(void) {
  return MM_BLOCK_TOTAL(MM_MIN_BLOCK_SIZE);
}

// header.size required to hold `payload` bytes, or 0 if that would overflow.
size_t mm_size_for(size_t payload) {
  if (payload > SIZE_MAX - MM_BLOCK_TAX - MM_ALIGNMENT) return 0;
  size_t need = mm_round_up(payload + MM_BLOCK_TAX, MM_ALIGNMENT);
  return need < MM_MIN_BLOCK_SIZE ? MM_MIN_BLOCK_SIZE : need;
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

  // Clear the arena once, up front. A payload checksum covers the whole
  // payload, including bytes the caller has not written yet, so those bytes
  // have to hold something defined -- otherwise the checksum is computed over
  // indeterminate memory. Paying for it here is O(arena) once rather than
  // O(size) on every allocation, and mmap-backed arenas arrive zeroed anyway.
  memset(g_arena.base, 0, heap_size);

  // One free block spanning everything that can be tiled, less the header and
  // footer. Any remainder at the tail is unusable and simply left alone.
  size_t usable = heap_size - sizeof(mm_header) - sizeof(mm_footer);
  usable -= usable % MM_ALIGNMENT;

  mm_header *first = (mm_header *)(void *)g_arena.base;
  memset(first, 0, sizeof(*first));
  first->magic = MM_HEADER_MAGIC;
  first->in_use = 0;
  first->size = usable;
  first->requested_size = 0;
  first->next = NULL;
  first->prev = NULL;
  first->payload_checksum = 0;
  mm_seal(first);

  g_arena.first = first;
  return 0;
}

// --- Block search ----------------------------------------------------------

// Smallest free block that fits, quarantining anything corrupt it walks over.
static mm_header *find_best_fit(size_t need) {
  mm_header *best = NULL;
  size_t best_size = SIZE_MAX;
  size_t budget = mm_max_blocks();

  mm_header *cur = g_arena.first;
  while (cur != NULL) {
    if (budget-- == 0) {
      // A link has gone circular. Stop rather than spin.
      mm_fail(MM_ERR_CORRUPT_LINKS);
      break;
    }
    mm_header *next = mm_checksum_ok(cur) ? cur->next : NULL;

    if (!mm_checksum_ok(cur) || !mm_footer_ok(cur)) {
      mm_quarantine(cur);
      cur = next;
      continue;
    }
    if (!cur->in_use && cur->size >= need && cur->size < best_size) {
      best = cur;
      best_size = cur->size;
      if (best_size == need) break;  // exact fit, stop looking
    }
    cur = cur->next;
  }
  return best;
}

static void coalesce_forward(mm_header *block);

// Points `node->prev` back at `owner` after the list around it changed.
//
// A neighbour that no longer validates must never be written through: its size
// field is what decides where its footer goes, so sealing a corrupted node
// scatters writes across the arena. Drop such a node from the list instead --
// the space it held is lost, which is the price of not trusting it.
static void relink_prev(mm_header *owner, mm_header *node) {
  if (node == NULL) return;
  if (!mm_checksum_ok(node) || !mm_footer_ok(node)) {
    // The list has to stop here: the damaged node's own size and links are
    // what would say where the next block begins, and neither can be trusted.
    // Everything from this point to the end of the arena becomes unreachable,
    // so account for all of it -- a truncation that is not counted looks
    // exactly like a heap that lost nothing.
    size_t offset = (size_t)((uint8_t *)node - g_arena.base);
    if (offset < g_arena.size) {
      g_arena.lost_bytes += g_arena.size - offset;
    }
    owner->next = NULL;
    mm_seal(owner);
    mm_fail(MM_ERR_CORRUPT_LINKS);
    return;
  }
  node->prev = owner;
  mm_seal(node);
}

// Splits `block` so it holds exactly `need`, provided the remainder is worth
// carving out. The tail becomes a free block linked in after it.
static void split_block(mm_header *block, size_t need) {
  size_t leftover = block->size - need;
  if (leftover < MM_BLOCK_TOTAL(MM_MIN_BLOCK_SIZE)) return;

  size_t tail_size = leftover - sizeof(mm_header) - sizeof(mm_footer);

  mm_header *tail =
      (mm_header *)(void *)((uint8_t *)block + MM_BLOCK_TOTAL(need));
  memset(tail, 0, sizeof(*tail));
  tail->magic = MM_HEADER_MAGIC;
  tail->in_use = 0;
  tail->size = tail_size;
  tail->requested_size = 0;
  tail->next = block->next;
  tail->prev = block;
  tail->payload_checksum = 0;

  block->next = tail;
  block->size = need;

  mm_seal(tail);
  mm_seal(block);
  relink_prev(tail, tail->next);

  // The block that followed may itself be free -- shrinking an allocation in
  // place is the case where that happens. Leaving two free blocks side by side
  // would fragment the arena permanently.
  coalesce_forward(tail);
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

  mm_header *block = find_best_fit(need);
  if (block == NULL) {
    MM_STAT_NOTE(MM_STAT_ALLOC_FAILED, 0);
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  split_block(block, need);

  block->in_use = 1;
  block->requested_size = size;

  // The clue is redundant with the fixed prefix; it exists so that a pointer
  // recovered from a caller can be cross-checked before the header is trusted.
  uint8_t *payload = mm_payload_of(block);
  size_t clue = MM_PREFIX;
  memcpy(payload - sizeof(size_t), &clue, sizeof(clue));

  mm_write_canary(block);
  // Deliberately left unestablished: the payload has not been written yet, so
  // there is nothing meaningful to checksum. Doing it here would also make
  // every allocation cost O(size).
  block->payload_checksum = 0;
  mm_seal(block);

  MM_STAT_NOTE(MM_STAT_ALLOC, size);
  MM_STAT_ADD(block);
  return payload;
}

// --- Release ---------------------------------------------------------------

// Merges `block` with the following block when both are free.
static void coalesce_forward(mm_header *block) {
  mm_header *next = block->next;
  if (next == NULL || next->in_use) return;
  if (!mm_checksum_ok(next) || !mm_footer_ok(next)) return;
  // Only merge blocks that are genuinely adjacent.
  if (mm_next_adjacent(block) != next) return;
  if (next->prev != block) return;

  block->size += MM_BLOCK_TOTAL(next->size);
  block->next = next->next;
  mm_seal(block);
  relink_prev(block, block->next);
}

void mm_free(void *ptr) {
  mm_clear_error();
  if (ptr == NULL) return;

  mm_header *block = mm_block_of(ptr);
  if (block == NULL) return;

  if (!mm_checksum_ok(block) || !mm_footer_ok(block)) {
    mm_quarantine(block);
    return;
  }
  if (!block->in_use) {
    mm_fail(MM_ERR_DOUBLE_FREE);
    return;
  }
  if (!mm_canary_ok(block)) {
    mm_quarantine(block);
    return;
  }

  MM_STAT_NOTE(MM_STAT_FREE, block->requested_size);
  MM_STAT_SUB(block);

  block->in_use = 0;
  block->requested_size = 0;
  block->payload_checksum = 0;
  mm_seal(block);

#ifdef MM_ZERO_ON_FREE
  // Scrub the payload region so freed contents cannot leak into a later
  // allocation. Stops short of the footer.
  memset((uint8_t *)block + sizeof(mm_header), 0, block->size);
  mm_seal(block);
#endif

  coalesce_forward(block);

  mm_header *prev = block->prev;
  if (prev != NULL && !prev->in_use && mm_checksum_ok(prev) &&
      mm_footer_ok(prev) && prev->next == block) {
    coalesce_forward(prev);
  }
}

// The payload checksum only means anything when every byte it covers has been
// written. Carrying it forward is therefore valid exactly when the surviving
// payload is entirely initialised -- that is, when the block is not growing.
// 0 marks it unestablished, a value mm_payload_checksum never returns.
static void carry_payload_checksum(mm_header *block, size_t old_size,
                                   uint32_t old_sum, size_t new_size) {
  if (old_sum != 0 && new_size <= old_size) {
    block->payload_checksum =
        mm_payload_checksum(mm_payload_of(block), new_size);
  } else {
    block->payload_checksum = 0;
  }
}

// --- Resize ----------------------------------------------------------------

void *mm_realloc(void *ptr, size_t new_size) {
  mm_clear_error();
  if (ptr == NULL) return mm_malloc(new_size);

  if (new_size == 0) {
    mm_free(ptr);
    return NULL;
  }

  mm_header *block = mm_block_of(ptr);
  if (block == NULL) return NULL;

  if (!mm_checksum_ok(block) || !mm_footer_ok(block)) {
    mm_quarantine(block);
    return NULL;
  }
  if (!block->in_use) {
    mm_fail(MM_ERR_DOUBLE_FREE);
    return NULL;
  }
  if (!mm_canary_ok(block)) {
    mm_quarantine(block);
    return NULL;
  }

  size_t need = mm_size_for(new_size);
  if (need == 0) {
    mm_fail(MM_ERR_NOMEM);
    return NULL;
  }

  MM_STAT_NOTE(MM_STAT_REALLOC, new_size);

  size_t old_size = block->requested_size;
  uint32_t old_sum = block->payload_checksum;
  uint8_t *payload = mm_payload_of(block);

  // Already big enough: shrink in place, handing back any surplus.
  if (block->size >= need) {
    MM_STAT_SUB(block);
    split_block(block, need);
    block->requested_size = new_size;
    mm_write_canary(block);
    carry_payload_checksum(block, old_size, old_sum, new_size);
    mm_seal(block);
    MM_STAT_ADD(block);
    return payload;
  }

  // Try to absorb the following block rather than move.
  mm_header *next = block->next;
  if (next != NULL && !next->in_use && mm_checksum_ok(next) &&
      mm_footer_ok(next) && mm_next_adjacent(block) == next &&
      next->prev == block && block->size + MM_BLOCK_TOTAL(next->size) >= need) {
    MM_STAT_SUB(block);
    coalesce_forward(block);
    split_block(block, need);
    block->requested_size = new_size;
    mm_write_canary(block);
    carry_payload_checksum(block, old_size, old_sum, new_size);
    mm_seal(block);
    MM_STAT_ADD(block);
    return payload;
  }

  // Fall back to allocate, copy, release. Copy before freeing so a failed
  // allocation leaves the original block untouched.
  void *fresh = mm_malloc(new_size);
  if (fresh == NULL) return NULL;

  size_t carry = old_size < new_size ? old_size : new_size;
  memcpy(fresh, payload, carry);

  mm_header *fresh_block = mm_block_of(fresh);
  if (fresh_block != NULL) {
    carry_payload_checksum(fresh_block, carry, old_sum, new_size);
    mm_seal(fresh_block);
  }

  mm_free(ptr);
  return fresh;
}
