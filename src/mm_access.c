// Validated reads and writes.

#include "mm_internal.h"

#include <string.h>

// Validates a block and the requested range against it. Returns the block, or
// NULL with the thread status set.
static mm_block *checked_block(const void *ptr, size_t offset, size_t len) {
  mm_block *b = mm_block_of(ptr);
  if (b == NULL) return NULL;

  if (!mm_header_ok(b)) {
    // Rescue puts the header back where the profile carries a mirror, and
    // surrenders the block's span where it does not. Nothing downstream is
    // written through a header that has not come back clean.
    if (!mm_rescue(b)) return NULL;
  }
  if (mm_is_quarantined(b)) {
    mm_fail(MM_ERR_QUARANTINED);
    return NULL;
  }
  if (!mm_is_used(b)) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  if (!mm_canary_ok(b)) {
    mm_quarantine(b);
    return NULL;
  }

  // Bounds check written so it cannot wrap: comparing offset and len against
  // the remaining space separately, rather than summing them first.
  size_t requested = mm_requested_size(b);
  if (offset > requested) {
    mm_fail(MM_ERR_OOB);
    return NULL;
  }
  if (len > requested - offset) {
    mm_fail(MM_ERR_OOB);
    return NULL;
  }
  return b;
}

int64_t mm_read(const void *ptr, size_t offset, void *buf, size_t len) {
  mm_clear_error();
  if (buf == NULL && len != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return -1;
  }

  mm_block *b = checked_block(ptr, offset, len);
  if (b == NULL) return -1;

#if MM_HAS_CRC
  // Payload integrity: only meaningful because every write goes through
  // mm_write, which keeps this checksum current. A zero checksum means the
  // block has not been written yet, so there is nothing to verify against --
  // reading uninitialised contents is the caller's business, not corruption.
  if (b->payload_crc != 0) {
    uint32_t got = mm_payload_crc(mm_payload_of(b), mm_requested_size(b));
    if (got != b->payload_crc) {
      mm_quarantine(b);
      mm_fail(MM_ERR_CORRUPT_PAYLOAD);
      return -1;
    }
  }
#endif

  if (len > 0) memcpy(buf, mm_payload_of(b) + offset, len);

  return (int64_t)len;
}

int64_t mm_write(void *ptr, size_t offset, const void *src, size_t len) {
  mm_clear_error();
  if (src == NULL && len != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return -1;
  }

  mm_block *b = checked_block(ptr, offset, len);
  if (b == NULL) return -1;

  uint8_t *payload = mm_payload_of(b);
  if (len > 0) memcpy(payload + offset, src, len);

  // The canary is checked before the write and rechecked after it, so an
  // overrun by this very call is caught here rather than papered over.
  if (!mm_canary_ok(b)) {
    mm_quarantine(b);
    return -1;
  }

#if MM_HAS_CRC
  b->payload_crc = mm_payload_crc(payload, mm_requested_size(b));
  mm_seal(b);
#endif

  return (int64_t)len;
}
