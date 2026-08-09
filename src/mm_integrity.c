// Checksums, canaries, block recovery, and heap-wide consistency checking.

#include "mm_internal.h"

#include <string.h>

mm_arena g_arena;

static _Thread_local mm_status_t g_last_error = MM_OK;

mm_status_t mm_last_error(void) { return g_last_error; }

mm_status_t mm_fail(mm_status_t status) {
  g_last_error = status;
  return status;
}

void mm_clear_error(void) { g_last_error = MM_OK; }

const char *mm_strerror(mm_status_t status) {
  switch (status) {
    case MM_OK:                  return "ok";
    case MM_ERR_NOT_INITIALIZED: return "allocator not initialized";
    case MM_ERR_BAD_ARENA:       return "arena rejected";
    case MM_ERR_INVALID_PTR:     return "pointer not from this arena";
    case MM_ERR_OOB:             return "range outside payload";
    case MM_ERR_CORRUPT_HEADER:  return "block header corrupted";
    case MM_ERR_CORRUPT_CANARY:  return "payload overflowed its block";
    case MM_ERR_CORRUPT_PAYLOAD: return "payload does not match checksum";
    case MM_ERR_CORRUPT_LINKS:   return "block list inconsistent";
    case MM_ERR_DOUBLE_FREE:     return "block already free";
    case MM_ERR_NOMEM:           return "no block large enough";
    case MM_ERR_QUARANTINED:     return "block quarantined";
  }
  return "unknown status";
}

// --- Checksums -------------------------------------------------------------

// FNV-1a over a byte range. Not cryptographic, and not the CRC32C the layout
// work will bring in -- but it is order-sensitive and mixes every input byte,
// which is what detection needs here.
static uint32_t fnv1a(const void *data, size_t len, uint32_t seed) {
  const uint8_t *p = (const uint8_t *)data;
  uint32_t h = seed;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 0x01000193u;
  }
  return h;
}

// Covers every header field except the two checksums themselves. Copying the
// fields into a local keeps this independent of struct padding, which is not
// guaranteed to be initialised.
uint32_t mm_checksum(const mm_header *block) {
  struct {
    uint32_t magic;
    uint32_t in_use;
    size_t size;
    size_t requested_size;
    uintptr_t next;
    uintptr_t prev;
  } fields;

  memset(&fields, 0, sizeof(fields));
  fields.magic = block->magic;
  fields.in_use = block->in_use;
  fields.size = block->size;
  fields.requested_size = block->requested_size;
  fields.next = (uintptr_t)block->next;
  fields.prev = (uintptr_t)block->prev;

  uint32_t h = fnv1a(&fields, sizeof(fields), 0x811C9DC5u);
  return h == 0 ? 1u : h;  // keep 0 free as "never computed"
}

uint32_t mm_payload_checksum(const void *payload, size_t len) {
  uint32_t h = fnv1a(payload, len, 0x811C9DC5u);
  return h == 0 ? 1u : h;
}

// --- Bounds ----------------------------------------------------------------

bool mm_in_arena(const void *p, size_t span) {
  if (g_arena.base == NULL || p == NULL) return false;
  const uint8_t *q = (const uint8_t *)p;
  if (q < g_arena.base) return false;
  // Compare as offsets so the arithmetic cannot run past the arena.
  size_t offset = (size_t)(q - g_arena.base);
  if (offset > g_arena.size) return false;
  return span <= g_arena.size - offset;
}

bool mm_is_block(const mm_header *block) {
  if (!mm_in_arena(block, sizeof(mm_header))) return false;
  if (((uintptr_t)block - (uintptr_t)g_arena.base) % MM_ALIGNMENT != 0) {
    return false;
  }
  if (block->magic != MM_HEADER_MAGIC) return false;
  if (block->size < MM_MIN_BLOCK_SIZE) return false;
  if (block->size % MM_ALIGNMENT != 0) return false;
  // The whole block, footer included, must lie inside the arena.
  return mm_in_arena(block, MM_BLOCK_TOTAL(block->size));
}

size_t mm_max_blocks(void) {
  if (g_arena.size == 0) return 0;
  return g_arena.size / MM_BLOCK_TOTAL(MM_MIN_BLOCK_SIZE) + 1;
}

// --- Validation ------------------------------------------------------------

bool mm_checksum_ok(const mm_header *block) {
  return mm_is_block(block) && block->checksum == mm_checksum(block);
}

bool mm_footer_ok(const mm_header *block) {
  if (!mm_is_block(block)) return false;
  const mm_footer *f = mm_footer_of(block);
  if (!mm_in_arena(f, sizeof(mm_footer))) return false;
  return memcmp(f, block, sizeof(mm_header)) == 0;
}

// The canary sits immediately after the payload, at an offset the caller
// chose, so it is not necessarily aligned. Always move it with memcpy.
void mm_read_canary(const mm_header *block, uint64_t *out) {
  memcpy(out, mm_payload_of(block) + block->requested_size, sizeof(*out));
}

void mm_write_canary(const mm_header *block) {
  uint64_t canary = MM_CANARY;
  memcpy(mm_payload_of(block) + block->requested_size, &canary,
         sizeof(canary));
}

bool mm_canary_ok(const mm_header *block) {
  if (!block->in_use) return true;  // free blocks carry no canary
  uint64_t seen = 0;
  mm_read_canary(block, &seen);
  return seen == MM_CANARY;
}

void mm_seal(mm_header *block) {
  block->checksum = mm_checksum(block);
  *mm_footer_of(block) = *block;
}

// --- Quarantine ------------------------------------------------------------

void mm_quarantine(mm_header *block) {
  if (!mm_in_arena(block, sizeof(mm_header))) return;

  mm_header *prev = block->prev;
  mm_header *next = block->next;

  // Only re-link through neighbours that still look sane; a corrupted block's
  // own pointers are exactly what cannot be trusted here.
  bool prev_ok = mm_checksum_ok(prev);
  bool next_ok = mm_checksum_ok(next);

  if (prev_ok) {
    prev->next = next_ok ? next : NULL;
    mm_seal(prev);
  }
  if (next_ok) {
    next->prev = prev_ok ? prev : NULL;
    mm_seal(next);
  }
  if (g_arena.first == block) {
    g_arena.first = next_ok ? next : NULL;
  }

  // Poison the magic so the block can never be mistaken for live again. The
  // space it occupied is deliberately not reclaimed.
  block->magic = 0xDEADDEADu;
  mm_fail(MM_ERR_QUARANTINED);
}

// --- Pointer recovery ------------------------------------------------------

mm_header *mm_block_of(const void *ptr) {
  if (g_arena.base == NULL) {
    mm_fail(MM_ERR_NOT_INITIALIZED);
    return NULL;
  }
  // Range-check the payload pointer BEFORE reading anything near it: the clue
  // lives just below it, and a foreign pointer must never be dereferenced.
  if (!mm_in_arena(ptr, 0)) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  size_t offset = (size_t)((const uint8_t *)ptr - g_arena.base);
  if (offset < MM_PREFIX || (offset - MM_PREFIX) % MM_ALIGNMENT != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }

  mm_header *block =
      (mm_header *)(void *)((uint8_t *)(uintptr_t)ptr - MM_PREFIX);
  if (!mm_is_block(block)) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }

  // Cross-check the clue written at allocation time. It is redundant with the
  // fixed prefix, which is what makes it useful as a corruption detector.
  size_t clue = 0;
  memcpy(&clue, (const uint8_t *)ptr - sizeof(size_t), sizeof(clue));
  if (clue != MM_PREFIX) {
    mm_fail(MM_ERR_CORRUPT_HEADER);
    return NULL;
  }
  return block;
}

// --- Public checks ---------------------------------------------------------

mm_status_t mm_verify(const void *ptr) {
  mm_clear_error();
  mm_header *block = mm_block_of(ptr);
  if (block == NULL) return mm_last_error();

  if (!mm_checksum_ok(block)) return mm_fail(MM_ERR_CORRUPT_HEADER);
  if (!mm_footer_ok(block)) return mm_fail(MM_ERR_CORRUPT_HEADER);
  if (!mm_canary_ok(block)) return mm_fail(MM_ERR_CORRUPT_CANARY);

  uint32_t want = block->payload_checksum;
  if (block->in_use && want != 0) {
    uint32_t got =
        mm_payload_checksum(mm_payload_of(block), block->requested_size);
    if (want != got) return mm_fail(MM_ERR_CORRUPT_PAYLOAD);
  }
  return MM_OK;
}

mm_status_t mm_check_heap(void) {
  mm_clear_error();
  if (g_arena.base == NULL) return mm_fail(MM_ERR_NOT_INITIALIZED);

  size_t budget = mm_max_blocks();
  size_t covered = 0;
  mm_header *prev = NULL;

  for (mm_header *b = g_arena.first; b != NULL; b = b->next) {
    if (budget-- == 0) return mm_fail(MM_ERR_CORRUPT_LINKS);
    if (!mm_is_block(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
    if (!mm_checksum_ok(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
    if (!mm_footer_ok(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
    if (!mm_canary_ok(b)) return mm_fail(MM_ERR_CORRUPT_CANARY);

    if (b->prev != prev) return mm_fail(MM_ERR_CORRUPT_LINKS);
    if (b->next != NULL && mm_next_adjacent(b) != b->next) {
      return mm_fail(MM_ERR_CORRUPT_LINKS);
    }
    // Adjacent free blocks mean a coalesce was missed.
    if (prev != NULL && !prev->in_use && !b->in_use) {
      return mm_fail(MM_ERR_CORRUPT_LINKS);
    }

    covered += MM_BLOCK_TOTAL(b->size);
    prev = b;
  }

  // The blocks must tile the arena exactly, modulo the tail too small to hold
  // one more block.
  if (covered > g_arena.size) return mm_fail(MM_ERR_CORRUPT_LINKS);
  if (g_arena.size - covered >= MM_BLOCK_TOTAL(MM_MIN_BLOCK_SIZE)) {
    return mm_fail(MM_ERR_CORRUPT_LINKS);
  }

  return MM_OK;
}
