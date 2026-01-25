#ifndef ALLOCATOR_H_
#define ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// --- Public API ---

// Initializes the memory allocator using the provided heap block.
// Sets up the initial free block, establishes the 5-byte pattern for
// radiation detection, and prepares the internal state.
//
// @param heap       Pointer to the start of the memory block.
// @param heap_size  Total size of the heap in bytes.
// @return           0 on success, -1 on failure (e.g., heap too small).
int mm_init(uint8_t *heap, size_t heap_size);

// Allocates a block of memory with the specified size.
// The returned pointer is guaranteed to be aligned to a 40-byte boundary
// relative to the start of the heap. Uses SECURE_WRITE to protect metadata.
//
// @param size Number of bytes to allocate.
// @return     Pointer to the payload on success, NULL on failure.
void *mm_malloc(size_t size);

// Reads data safely from an allocated block.
// Performs bounds checking against requested_size and verifies integrity
// (checksums, footers) before reading to prevent accessing corrupted memory.
//
// @param ptr    Pointer to the allocated block.
// @param offset Offset within the block to start reading.
// @param buf    Destination buffer.
// @param len    Number of bytes to read.
// @return       Bytes read on success, -1 on corruption or invalid pointer.
int mm_read(void *ptr, size_t offset, void *buf, size_t len);

// Writes data safely to an allocated block.
// Includes Brownout Protection: verify-after-write loops ensure data is
// correctly persisted even during power fluctuations.
//
// @param ptr    Pointer to the allocated block.
// @param offset Offset within the block to start writing.
// @param src    Source buffer.
// @param len    Number of bytes to write.
// @return       Bytes written on success, -1 on corruption or invalid pointer.
int mm_write(void *ptr, size_t offset, const void *src, size_t len);

// Frees a previously allocated block.
// Coalesces adjacent free blocks to reduce fragmentation and sanitizes
// the memory (restoring the pattern) to prevent data leaks.
// Safely ignores NULL pointers and detects double-frees.
//
// @param ptr Pointer to the block to free.
void mm_free(void *ptr);

// Resizes an existing allocation to new_size.
// Optimizes by attempting to shrink or grow in-place before resorting to
// a full malloc-memcpy-free cycle.
//
// @param ptr      Pointer to the existing block.
// @param new_size New requested size in bytes.
// @return         Pointer to new memory (may be same as ptr) or NULL on
//                 failure.
void *mm_realloc(void *ptr, size_t new_size);

// --- Internal Data Structures ---

// Block Header Structure.
// Contains metadata required to manage the block and verify its integrity.
typedef struct header {
  uint32_t magic;           // Magic number for header validation
  int in_use;               // 1 if allocated, 0 if free
  size_t size;              // Total block size (Header + Payload + Pad + Ftr)
  size_t requested_size;    // Exact payload size requested by user (for Canary)
  struct header *next;      // Pointer to next block in the list
  struct header *prev;      // Pointer to previous block in the list
  uint32_t checksum;        // XOR checksum for metadata integrity
  uint32_t payload_checksum;  // Checksum of the payload data
} header;

// Footer is structurally identical to header.
// Used for mirrored metadata verification at the end of the block.
typedef header footer;

// --- Helper Functions (Internal Use) ---

// Uses First-Fit/Best-Fit strategy to locate a free block of sufficient size.
header *find_best_block(size_t size);

// Calculates the XOR checksum of a header's fields.
uint32_t calculate_checksum(header *ptr);

// Calculates the checksum for the payload data to detect corruption.
uint32_t calculate_payload_checksum(void *ptr, size_t size);

// Recovers the header pointer from a user payload pointer using the Clue.
header *obtain_block(void *ptr);

// Verifies if the header's checksum matches its contents.
int valid_checksum(header *ptr);

// Verifies if the mirrored footer matches the header.
int valid_footer(header *header_ptr);

// Checks the Payload Canary to detect buffer overflows.
int valid_canary(void *ptr);

// Verifies the consistency of the doubly linked list topology.
int check_consistency(header *ptr);

// Quarantines a corrupted block by safely unlinking it from the list.
void remove_corrupted(header *ptr);

// Checks if a pointer lies within the valid heap boundaries.
int safe_ptr(header *ptr);

// Updates the checksum and mirrored footer after modifying a header.
void fix_linkage(header *header_ptr);

#endif  // ALLOCATOR_H_
