// Validated reads and writes.

#include "mm_internal.h"

#include <string.h>

// Validates a block and the requested range against it. Returns the block, or
// NULL with the thread status set.
static mm_header *checked_block(const void *ptr, size_t offset, size_t len) {
  mm_header *block = mm_block_of(ptr);
  if (block == NULL) return NULL;

  if (!mm_checksum_ok(block) || !mm_footer_ok(block)) {
    mm_quarantine(block);
    return NULL;
  }
  if (!block->in_use) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  if (!mm_canary_ok(block)) {
    mm_quarantine(block);
    return NULL;
  }

  // Bounds check written so it cannot wrap: comparing offset and len against
  // the remaining space separately, rather than summing them first.
  if (offset > block->requested_size) {
    mm_fail(MM_ERR_OOB);
    return NULL;
  }
  if (len > block->requested_size - offset) {
    mm_fail(MM_ERR_OOB);
    return NULL;
  }
  return block;
}

int64_t mm_read(const void *ptr, size_t offset, void *buf, size_t len) {
  mm_clear_error();
  if (buf == NULL && len != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return -1;
  }

  mm_header *block = checked_block(ptr, offset, len);
  if (block == NULL) return -1;

  // Payload integrity: only meaningful because every write goes through
  // mm_write, which keeps this checksum current. A zero checksum means the
  // block has not been written yet, so there is nothing to verify against --
  // reading uninitialised contents is the caller's business, not corruption.
  uint32_t expect = block->payload_checksum;
  if (expect != 0) {
    uint32_t actual =
        mm_payload_checksum(mm_payload_of(block), block->requested_size);
    if (expect != actual) {
      mm_quarantine(block);
      mm_fail(MM_ERR_CORRUPT_PAYLOAD);
      return -1;
    }
  }

  if (len > 0) memcpy(buf, mm_payload_of(block) + offset, len);

  return (int64_t)len;
}

int64_t mm_write(void *ptr, size_t offset, const void *src, size_t len) {
  mm_clear_error();
  if (src == NULL && len != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return -1;
  }

  mm_header *block = checked_block(ptr, offset, len);
  if (block == NULL) return -1;

  uint8_t *payload = mm_payload_of(block);
  if (len > 0) memcpy(payload + offset, src, len);

  // The canary is checked before the write and rewritten after it, so an
  // overrun by this very call is caught on the next access rather than being
  // papered over.
  if (!mm_canary_ok(block)) {
    mm_quarantine(block);
    return -1;
  }

  block->payload_checksum = mm_payload_checksum(payload,
                                                block->requested_size);
  mm_seal(block);

  return (int64_t)len;
}
