#include "mars/allocator.h"

#include <string.h>

#define ALIGNMENT 40
#define MAGIC_NUMBER 0xDEADBEEF
#define CANARY 0xCAFEBABECAFEBABEULL
#define HEADER_MAGIC 0xBADC0FFE

// Macro: SECURE_WRITE
// Purpose: Protects against Brownouts (power dips during write
// operations).
// Strategy: Read-After-Write (RAW) verification.
//
// We attempt to write the value up to 3 times. After every write, we
// immediately read the memory back to ensure the electrons settled
// correctly.
// Returns: 1 on success, 0 on persistent hardware failure.
#define SECURE_WRITE(type, addr, val) ({                    \
  int success = 0;                                          \
  for (int i = 0; i < 3; i++) {                             \
    *(addr) = (val);                                        \
    volatile type read_back = *(addr);                      \
    if (read_back == (val)) {                               \
      success = 1;                                          \
      break;                                                \
    }                                                       \
  }                                                         \
  success;                                                  \
})

/* Global heap state tracking */
static uint8_t *g_heap;
static size_t g_heap_size;
static header *g_first_block;
// Stores the OS-provided pattern for verification
static uint8_t g_pattern[5];

/* Initialize the allocator
 *
 * Sets up the first free block spanning the entire heap and stores the
 * initial 5-byte pattern for later use in sanitization.
 */
int mm_init(uint8_t *heap, size_t heap_size) {
  /* We need enough space for at least one header and one footer. */
  if (!heap || heap_size < sizeof(header) + sizeof(footer)) return -1;

  g_heap = heap;
  g_heap_size = heap_size;

  /* Capture the OS pattern from the first 5 bytes. */
  for (int i = 0; i < 5; i++) {
    g_pattern[i] = heap[i];
  }

  /* Verify the pattern exists across the whole heap (Radiation check). */
  for (size_t i = 0; i < heap_size; i++) {
    if (heap[i] != g_pattern[i % 5]) return -1;
  }

  /* Initialize the first giant free block. */
  g_first_block = (header *)g_heap;

  /* Use SECURE_WRITE for all critical metadata initialization to prevent
   * corruption during startup brownouts.
   */
  if (!SECURE_WRITE(uint32_t, &g_first_block->magic, HEADER_MAGIC)) return -1;
  if (!SECURE_WRITE(int, &g_first_block->in_use, 0)) return -1;
  /* Size = Total - Overhead (Header + Footer) */
  if (!SECURE_WRITE(size_t, &g_first_block->size,
                    heap_size - sizeof(header) - sizeof(footer))) {
    return -1;
  }
  if (!SECURE_WRITE(size_t, &g_first_block->requested_size, 0)) return -1;
  if (!SECURE_WRITE(header*, &g_first_block->next, NULL)) return -1;
  if (!SECURE_WRITE(header*, &g_first_block->prev, NULL)) return -1;
  if (!SECURE_WRITE(uint32_t, &g_first_block->checksum,
                    calculate_checksum(g_first_block))) {
    return -1;
  }

  /* Write the Mirrored Footer at the end of the block. This allows us to
   * verify block integrity from both ends.
   */
  footer *footer_address = (footer *)((uint8_t *)g_first_block +
                                      sizeof(header) + g_first_block->size);

  *footer_address = *g_first_block;

  return 0;
}

/* Allocates a block of memory of the given size.
 *
 * Uses Best-Fit strategy and handles 40-byte relative alignment.
 */
void *mm_malloc(size_t size) {
  if (size <= 0) return NULL;

  /* Find the block that fits the requested size with minimal waste. */
  header *best = find_best_block(size);
  if (!best) return NULL;

  /* Calculate alignment padding.
   * The user's payload pointer must be 40-byte aligned relative to g_heap.
   */
  uint8_t *padding_start = (uint8_t *)best + sizeof(header);
  size_t alignment =
      (size_t)(padding_start - g_heap) + sizeof(size_t);
  size_t padding =
      (ALIGNMENT - ((size_t)alignment % ALIGNMENT)) % ALIGNMENT;
  uint8_t *return_ptr = (uint8_t *)(g_heap + alignment + padding);

  /* Calculate total space required including overheads.
   * Clue (size_t) + Padding + Payload + Canary (uint64_t)
   */
  size_t total_needed = sizeof(size_t) + padding + size + sizeof(uint64_t);

  /* --- SPLIT LOGIC ---
   * If the block is big enough to hold the request + a new free block,
   * split it.
   */
  if (best->size > total_needed + sizeof(header) + sizeof(footer)) {
    /* Pointer arithmetic to find where the new free block starts. */
    header *new_header = (header *)((uint8_t *)best + sizeof(header) +
                                    total_needed + sizeof(footer));

    /* Initialize the new free block header securely. */
    if (!SECURE_WRITE(uint32_t, &new_header->magic, HEADER_MAGIC)) return NULL;
    if (!SECURE_WRITE(header*, &new_header->next, best->next)) return NULL;
    if (!SECURE_WRITE(header*, &new_header->prev, best)) return NULL;
    /* Remainder size calculation */
    if (!SECURE_WRITE(size_t, &new_header->size,
                      best->size - sizeof(header) - total_needed -
                          sizeof(footer))) {
      return NULL;
    }
    if (!SECURE_WRITE(size_t, &new_header->requested_size, 0)) return NULL;
    if (!SECURE_WRITE(int, &new_header->in_use, 0)) return NULL;
    if (!SECURE_WRITE(uint32_t, &new_header->checksum,
                      calculate_checksum(new_header))) {
      return NULL;
    }

    /* Mirror the footer for the new free block. */
    footer *footer_address = (footer *)((uint8_t *)new_header +
                                        sizeof(header) + new_header->size);
    *footer_address = *new_header;

    /* Update pointers in the linked list. */
    if (best->next) {
      if (!SECURE_WRITE(header*, &best->next->prev, new_header)) return NULL;
      /* Re-seal the neighbor because we changed its 'prev' pointer. */
      fix_linkage(best->next);
    }
    if (!SECURE_WRITE(header*, &best->next, new_header)) return NULL;
    if (!SECURE_WRITE(size_t, &best->size, total_needed)) return NULL;
  }

  /* Write the "Clue" (Offset to header) immediately before the returned
   * ptr. This allows mm_free to find the header given just the void* ptr.
   */
  size_t offset = (size_t)return_ptr - (size_t)best;
  *((size_t *)(return_ptr - sizeof(size_t))) = offset;

  /* --- FINALIZE ALLOCATION ---
   * Mark the block as in-use and seal it with a checksum.
   */
  if (!SECURE_WRITE(int, &best->in_use, 1)) return NULL;
  if (!SECURE_WRITE(size_t, &best->requested_size, size)) return NULL;
  if (!SECURE_WRITE(uint32_t, &best->checksum, calculate_checksum(best))) {
    return NULL;
  }

  /* Write the allocated block's footer. */
  footer *new_footer =
      (footer *)((uint8_t *)best + sizeof(header) + best->size);
  *new_footer = *best;

  /* Write the Payload Canary immediately after the user's data. This
   * detects buffer overflows.
   */
  uint64_t *canary_start = (uint64_t *)(return_ptr + size);
  *canary_start = CANARY;

  return return_ptr;
}

/* Search the list for the Best Fit block (smallest block that fits).
 *
 * Also performs integrity checks on every block visited to detect
 * radiation.
 */
header *find_best_block(size_t size) {
  header *best = NULL;
  size_t min_leftover = SIZE_MAX;

  header *current = g_first_block;
  while (safe_ptr(current)) {
    /* Integrity Check: Verify Checksum and Mirrored Footer. If corruption
     * is detected, we quarantine the block immediately.
     */
    if (!valid_checksum(current) || !valid_footer(current)) {
      remove_corrupted(current);
      current = current->next;
      continue;
    }

    /* Check if block is free */
    if (!current->in_use) {
      /* Calculate exactly how much space is needed including alignment. */
      uint8_t *padding_start = (uint8_t *)current + sizeof(header);
      size_t alignment =
          (size_t)(padding_start - g_heap) + sizeof(size_t);
      size_t padding =
          (ALIGNMENT - ((size_t)alignment % ALIGNMENT)) % ALIGNMENT;
      size_t total_needed =
          sizeof(size_t) + padding + size + sizeof(uint64_t);

      if (current->size >= total_needed) {
        size_t leftover = current->size - total_needed;

        /* Best Fit: Is this leftover smaller than what we've seen before? */
        if (leftover < min_leftover) {
          best = current;
          min_leftover = leftover;

          /* Perfect fit optimisation */
          if (leftover == 0) return best;
        }
      }
    }
    current = current->next;
  }
  return best;
}

/* Reads data safely from the allocated block. Validates block integrity
 * before reading.
 */
int mm_read(void *ptr, size_t offset, void *buf, size_t len) {
  if (!ptr || !buf || !g_heap) return -1;

  header *block = obtain_block(ptr);
  if (!safe_ptr(block)) return -1;

  /* Radiation Check: Ensure metadata hasn't been flipped. */
  if (!valid_checksum(block) || !valid_footer(block)) {
    remove_corrupted(block);
    return -1;
  }

  /* Payload Checksum Verification */
  uint32_t current_data_hash =
      calculate_payload_checksum(ptr, block->requested_size);
  if (current_data_hash != block->payload_checksum) {
    /* Data corruption detected */
    remove_corrupted(block);
    return -1;
  }

  /* Bounds Check: Ensure we don't read past requested size. */
  if (!block->in_use || offset + len > block->requested_size) return -1;

  /* Simple read - Brownout protection not needed for reads. */
  memcpy(buf, (uint8_t *)ptr + offset, len);

  return (int)len; /* Return number of bytes read */
}

/* Writes data safely to the allocated block. Includes Brownout Protection
 * (Read-After-Write Verification).
 */
int mm_write(void *ptr, size_t offset, const void *src, size_t len) {
  if (!ptr || !src || !g_heap) return -1;

  header *block = obtain_block(ptr);
  if (!safe_ptr(block)) return -1;

  /* Integrity Check */
  if (!valid_checksum(block) || !valid_footer(block)) {
    remove_corrupted(block);
    return -1;
  }

  /* Bounds Check */
  if (!block->in_use || offset + len > block->requested_size) return -1;
  if (offset == 0 && len != block->requested_size) {
    /* Writing the entire payload is required for offset 0 writes */
    return -1;
  }

  /* Brownout Loop: Write -> Read -> Verify -> Retry
   * This ensures the payload is actually persisted to memory.
   */
  int attempts = 0;
  int success = 0;
  while (attempts < 3) {
    memcpy((uint8_t *)ptr + offset, src, len);
    /* Compare memory against source */
    if (memcmp((uint8_t *)ptr + offset, src, len) == 0) {
      success = 1;
      break;
    }
    attempts++;
  }

  if (!success) {
    remove_corrupted(block);  /* Persistent write failure, quarantine block */
    return -1;               /* Failed to write reliably */
  }

  /* Update payload checksum */
  block->payload_checksum =
      calculate_payload_checksum(ptr, block->requested_size);
  /* Update header checksum */
  block->checksum = calculate_checksum(block);

  footer *block_footer =
      (footer *)((uint8_t *)block + sizeof(header) + block->size);
  if (safe_ptr(block_footer)) {
    *block_footer = *block; /* Update footer to match header */
  }

  return (int)len;
}

/* Frees the allocated block and coalesces with neighbors. Performs
 * sanitation (pattern reset) and checks canaries.
 */
void mm_free(void *ptr) {
  if (!ptr) return;

  header *block = obtain_block(ptr);
  if (!safe_ptr(block)) return;

  /* Full Integrity Check: Checksum, Footer, and Payload Canary. */
  if (!valid_checksum(block) || !valid_footer(block) || !valid_canary(ptr)) {
    remove_corrupted(block);
    return;
  }

  if (!block->in_use) return; /* Double-free detection */

  /* --- COALESCE NEXT ---
   * If the next block is free, merge with it.
   */
  if (safe_ptr(block->next)) {
    if (!valid_checksum(block->next) || !valid_footer(block->next)) {
      remove_corrupted(block->next); /* Neighbor corrupted, remove it. */
    } else if (!block->next->in_use) {
      /* Calculate merged size */
      block->size += block->next->size + sizeof(header) + sizeof(footer);
      block->next = block->next->next;

      /* Update links securely */
      if (block->next) {
        block->next->prev = block;
        fix_linkage(block->next);
      }
    }
  }

  /* --- COALESCE PREV ---
   * If the previous block is free, merge with it.
   */
  if (safe_ptr(block->prev)) {
    if (!valid_checksum(block->prev) || !valid_footer(block->prev)) {
      remove_corrupted(block->prev);
    } else if (!block->prev->in_use) {
      block = block->prev;
      block->size += block->next->size + sizeof(header) + sizeof(footer);
      block->next = block->next->next;
      if (block->next) {
        block->next->prev = block;
        fix_linkage(block->next);
      }
    }
  }

  /* Mark as free */
  block->in_use = 0;
  block->requested_size = 0;
  block->checksum = calculate_checksum(block);

  /* Update footer to match new header state */
  footer *footer_address =
      (footer *)((uint8_t *)block + sizeof(header) + block->size);

  footer_address->in_use = 0;
  footer_address->requested_size = 0;
  footer_address->size = block->size;
  footer_address->next = block->next;
  footer_address->prev = block->prev;
  footer_address->checksum = calculate_checksum(footer_address);

  /* Sanitize Memory: Fill payload with 5-byte pattern */
  uint8_t *payload_start = (uint8_t *)block + sizeof(header);

  for (size_t i = 0; i < block->size; i++) {
    size_t global_offset = (size_t)(payload_start - g_heap) + i;
    payload_start[i] = g_pattern[global_offset % 5];
  }
}

/* Resizes a block securely.
 *
 * Implements In-Place optimizations (Growing/Shrinking) to minimize copy
 * ops.
 */
void *mm_realloc(void *ptr, size_t new_size) {
  if (!ptr) return mm_malloc(new_size);

  header *block = obtain_block(ptr);
  if (!safe_ptr(block)) return NULL;

  if (!valid_checksum(block) || !valid_footer(block) || !valid_canary(ptr)) {
    remove_corrupted(block);
    return NULL;
  }

  /* Recover padding to determine total needed space for the new size */
  size_t current_padding =
      (size_t)ptr - (size_t)block - sizeof(header) - sizeof(size_t);
  size_t new_total_needed =
      current_padding + sizeof(size_t) + new_size + sizeof(uint64_t);

  /* --- OPTIMIZATION 1: SHRINKING --- */
  if (block->size >= new_total_needed) {
    /* Case A: True Split (Remainder is large enough for a new block) */
    if (block->size >= new_total_needed + sizeof(header) + sizeof(footer)) {
      size_t old_block_size = block->size;

      /* Update current block size */
      if (!SECURE_WRITE(size_t, &block->size, new_total_needed)) return NULL;
      if (!SECURE_WRITE(size_t, &block->requested_size, new_size)) return NULL;
      if (!SECURE_WRITE(uint32_t, &block->checksum,
                        calculate_checksum(block))) {
        return NULL;
      }

      /* Move Canary */
      uint64_t *new_canary = (uint64_t *)((uint8_t *)ptr + new_size);
      *new_canary = CANARY;

      /* Create new Footer for current block */
      footer *new_footer =
          (footer *)((uint8_t *)block + sizeof(header) + block->size);
      *new_footer = *block;

      /* Create New Free Block (Remainder) */
      header *free_block =
          (header *)((uint8_t *)block + sizeof(header) + block->size +
                     sizeof(footer));
      /* Initialize free block metadata securely */
      SECURE_WRITE(uint32_t, &free_block->magic, HEADER_MAGIC);
      SECURE_WRITE(int, &free_block->in_use, 0);
      SECURE_WRITE(size_t, &free_block->requested_size, 0);
      SECURE_WRITE(size_t, &free_block->size,
                   old_block_size - block->size - sizeof(footer) -
                       sizeof(header));
      SECURE_WRITE(header*, &free_block->next, block->next);
      SECURE_WRITE(header*, &free_block->prev, block);
      SECURE_WRITE(uint32_t, &free_block->checksum,
                   calculate_checksum(free_block));

      footer *free_footer =
          (footer *)((uint8_t *)free_block + sizeof(header) + free_block->size);
      *free_footer = *free_block;

      /* Update links to insert free block */
      if (block->next) {
        SECURE_WRITE(header*, &block->next->prev, free_block);
        fix_linkage(block->next);
      }
      SECURE_WRITE(header*, &block->next, free_block);
      fix_linkage(block);

      /* Attempt to coalesce the new free block with its neighbor */
      if (safe_ptr(free_block->next)) {
        if (!free_block->next->in_use) {
          free_block->size += sizeof(footer) + sizeof(header) +
                              free_block->next->size;
          free_block->next = free_block->next->next;
          free_block->checksum = calculate_checksum(free_block);
          if (safe_ptr(free_block->next)) {
            free_block->next->prev = free_block;
            fix_linkage(free_block->next);
          }
          footer *merged_footer =
              (footer *)((uint8_t *)free_block + sizeof(header) +
                         free_block->size);
          *merged_footer = *free_block;
        }
      }
    } else {
      /* Case B: Lazy Shrink (Remainder too small to split)
       * Just update requested_size and move canary.
       */
      if (!SECURE_WRITE(size_t, &block->requested_size, new_size)) return NULL;
      if (!SECURE_WRITE(uint32_t, &block->checksum,
                        calculate_checksum(block))) {
        return NULL;
      }

      uint64_t *canary_start = (uint64_t *)((uint8_t *)ptr + new_size);
      *canary_start = CANARY;

      footer *block_footer =
          (footer *)((uint8_t *)block + sizeof(header) + block->size);
      block_footer->requested_size = new_size;
      block_footer->checksum = calculate_checksum(block_footer);
    }

    return ptr;
  }

  /* --- OPTIMIZATION 2: GROWING (Merge with next) --- */
  if (safe_ptr(block->next)) {
    if (!block->next->in_use &&
        (block->size + sizeof(footer) + sizeof(header) + block->next->size) >=
            new_total_needed) {
      /* Check if we can grow AND split the neighbor */
      if ((block->size + block->next->size) > new_total_needed) {
        size_t old_free_size = block->next->size;
        header *free_block_next = block->next->next;

        /* Calculate position of new free block */
        header *new_free_block = (header *)((uint8_t *)block + sizeof(header) +
                                            new_total_needed + sizeof(footer));

        /* Setup new free block */
        SECURE_WRITE(uint32_t, &new_free_block->magic, HEADER_MAGIC);
        SECURE_WRITE(int, &new_free_block->in_use, 0);
        SECURE_WRITE(size_t, &new_free_block->requested_size, 0);
        SECURE_WRITE(size_t, &new_free_block->size,
                     old_free_size - (new_size - block->requested_size));
        SECURE_WRITE(header*, &new_free_block->prev, block);
        SECURE_WRITE(header*, &new_free_block->next, free_block_next);
        SECURE_WRITE(uint32_t, &new_free_block->checksum,
                     calculate_checksum(new_free_block));

        if (safe_ptr(new_free_block->next)) {
          new_free_block->next->prev = new_free_block;
          fix_linkage(new_free_block->next);
        }

        footer *free_block_footer =
            (footer *)((uint8_t *)new_free_block + sizeof(header) +
                       new_free_block->size);
        *free_block_footer = *new_free_block;

        /* Update current block to grown size */
        block->size += new_size - block->requested_size;
        block->requested_size = new_size;
        block->next = new_free_block;

      } else {
        /* Full Merge: Consume the entire next block */
        block->size += sizeof(footer) + sizeof(header) + block->next->size;
        block->requested_size = new_size;
        block->next = block->next->next;
        if (safe_ptr(block->next)) {
          block->next->prev = block;
          fix_linkage(block->next);
        }
      }

      block->checksum = calculate_checksum(block);

      uint64_t *canary_start = (uint64_t *)((uint8_t *)ptr + new_size);
      *canary_start = CANARY;

      footer *block_footer =
          (footer *)((uint8_t *)block + sizeof(header) + block->size);
      *block_footer = *block;

      return ptr;
    }
  }

  /* --- FALLBACK: Malloc - Copy - Free --- */
  void *new_ptr = mm_malloc(new_size);
  if (new_ptr == NULL) return NULL;

  memcpy(new_ptr, ptr, block->requested_size);
  mm_free(ptr);

  return new_ptr;
}

/* Checksum calculation to detect bit flips (Radiation).
 * XORs all critical fields with a magic number.
 */
uint32_t calculate_checksum(header *ptr) {
  uint32_t hash = MAGIC_NUMBER;

  hash ^= (uint32_t)(ptr->size);
  hash = (hash << 5) | (hash >> 27);

  hash ^= (uint32_t)(ptr->requested_size);
  hash = (hash << 7) | (hash >> 25);

  hash ^= (uint32_t)(ptr->in_use) * 0x9e3779b9; /* Multiply by golden ratio */
  hash = (hash << 11) | (hash >> 21);

  /* We XOR the upper and lower 32 bits to ensure we catch flips in the high
   * bits
   */
  uintptr_t next_val = (uintptr_t)ptr->next;
  hash ^= (uint32_t)(next_val & 0xFFFFFFFF) ^ (uint32_t)(next_val >> 32);
  hash = (hash << 13) | (hash >> 19);

  uintptr_t prev_val = (uintptr_t)ptr->prev;
  hash ^= (uint32_t)(prev_val & 0xFFFFFFFF) ^ (uint32_t)(prev_val >> 32);
  hash = (hash << 17) | (hash >> 15);

  /* Include the payload checksum in the header's integrity check. This links
   * the metadata to the data; if the data checksum changes without the
   * header being updated, this check will fail.
   */
  hash ^= ptr->payload_checksum;
  hash = (hash << 19) | (hash >> 13); /* Rotate again to mix bits */

  return hash;
}

/* Calculates checksum for the payload data */
uint32_t calculate_payload_checksum(void *ptr, size_t size) {
  uint32_t hash = 0xCAFEBABE; /* Different seed than header */
  uint8_t *data = (uint8_t *)ptr;

  for (size_t i = 0; i < size; i++) {
    hash ^= data[i];
    hash = (hash << 5) | (hash >> 27); /* Rotate left by 5 */
  }
  return hash;
}

/* Retrieves the header from the user payload pointer using the Clue. */
header *obtain_block(void *ptr) {
  size_t header_offset =
      *((size_t *)((uint8_t *)ptr - sizeof(size_t)));

  if (header_offset < sizeof(header) || header_offset > g_heap_size) {
    return NULL; /* Detected corruption in the clue */
  }

  header *block = (header *)((uint8_t *)ptr - header_offset);

  return block;
}

/* Validates the header checksum. */
int valid_checksum(header *ptr) {
  if (ptr->checksum == calculate_checksum(ptr)) return 1;
  return 0;
}

/* Validates that the footer matches the header (Mirrored Footer).
 * Checks for radiation damage at the end of the block.
 */
int valid_footer(header *header_ptr) {
  /* Basic sanity check */
  if (header_ptr->size >= g_heap_size) return 0;

  footer *footer_ptr =
      (footer *)((uint8_t *)header_ptr + sizeof(header) + header_ptr->size);
  if (!safe_ptr(footer_ptr)) return 0;

  if (calculate_checksum(header_ptr) == calculate_checksum(footer_ptr)) {
    return 1;
  }
  return 0;
}

/* Validates the Payload Canary to detect buffer overflows. */
int valid_canary(void *payload_ptr) {
  size_t header_offset =
      *((size_t *)((uint8_t *)payload_ptr - sizeof(size_t)));
  header *block = (header *)((uint8_t *)payload_ptr - header_offset);

  uint64_t *canary_start =
      (uint64_t *)((uint8_t *)payload_ptr + block->requested_size);
  if (*canary_start == CANARY) return 1;
  return 0;
}

void remove_corrupted(header *ptr) {
  /* Safety check: Is ptr even in our heap? */
  if (!ptr || (uint8_t *)ptr < g_heap ||
      (uint8_t *)ptr + sizeof(header) > g_heap + g_heap_size) {
    return;
  }

  /* Try to get reliable prev/next from the footer (if it's intact)
   * The footer is at the END of the block, so it's less likely to be
   * corrupted at the same time as the header
   */
  header *reliable_prev = NULL;
  header *reliable_next = NULL;

  /* Attempt to read from footer if size seems reasonable */
  if (ptr->size < g_heap_size && ptr->size > 0) {
    footer *ftr = (footer *)((uint8_t *)ptr + sizeof(header) + ptr->size);
    if ((uint8_t *)ftr + sizeof(footer) <= g_heap + g_heap_size) {
      /* Use footer's pointers (might be more reliable) */
      reliable_prev = ftr->prev;
      reliable_next = ftr->next;
    }
  }

  /* Fallback: use header's pointers if footer lookup failed */
  if (!reliable_prev && !reliable_next) {
    reliable_prev = ptr->prev;
    reliable_next = ptr->next;
  }

  /* Mark block as permanently corrupted (poison it)
   * Use a different magic number so it won't pass safe_ptr checks
   */
  SECURE_WRITE(uint32_t, &ptr->magic, 0xDEADDEAD);
  /* Mark as "in use" so it won't be allocated */
  SECURE_WRITE(int, &ptr->in_use, 1);

  /* Unlink from the list */
  if (reliable_prev == NULL) {
    /* This was the first block */
    if (safe_ptr(reliable_next)) {
      SECURE_WRITE(header*, &g_first_block, reliable_next);
      SECURE_WRITE(header*, &reliable_next->prev, NULL);
      fix_linkage(reliable_next);
    } else {
      SECURE_WRITE(header*, &g_first_block, NULL);
    }
  } else {
    /* Middle or end block */
    if (safe_ptr(reliable_prev)) {
      SECURE_WRITE(header*, &reliable_prev->next, reliable_next);
      fix_linkage(reliable_prev);
    }

    if (safe_ptr(reliable_next)) {
      SECURE_WRITE(header*, &reliable_next->prev, reliable_prev);
      fix_linkage(reliable_next);
    }
  }
}

/* Checks if a pointer is within the valid heap range. */
int safe_ptr(header *ptr) {
  if (!ptr) return 0;

  if ((uint8_t *)ptr < g_heap ||
      (uint8_t *)ptr + sizeof(header) > g_heap + g_heap_size) {
    return 0;
  }

  /* Additional sanity check: Validate magic number */
  if (ptr->magic != HEADER_MAGIC) return 0;

  return 1;
}

/* Updates the checksum and mirrored footer of a block after modification. */
void fix_linkage(header *header_ptr) {
  header_ptr->checksum = calculate_checksum(header_ptr);
  footer *footer_address =
      (footer *)((uint8_t *)header_ptr + sizeof(header) + header_ptr->size);
  if (safe_ptr(footer_address)) *footer_address = *header_ptr;
}
