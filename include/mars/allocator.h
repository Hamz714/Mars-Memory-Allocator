// A fault-tolerant memory allocator.
//
// The allocator manages a caller-supplied arena. Every block carries integrity
// metadata -- a checksummed header, a mirrored footer, and a canary past the
// payload -- which is verified on each access, so that memory corrupted after
// the fact is detected rather than silently used.
//
// Payload integrity is only meaningful when reads and writes go through
// mm_read and mm_write: a raw pointer handed to the caller can be written
// behind the allocator's back, at which point any payload checksum is stale.

#ifndef MARS_ALLOCATOR_H_
#define MARS_ALLOCATOR_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Status reporting ------------------------------------------------------

typedef enum mm_status {
  MM_OK = 0,
  MM_ERR_NOT_INITIALIZED,  // no arena has been installed
  MM_ERR_BAD_ARENA,        // arena rejected: too small, misaligned, or null
  MM_ERR_INVALID_PTR,      // pointer did not come from this arena
  MM_ERR_OOB,              // offset/length outside the payload
  MM_ERR_CORRUPT_HEADER,   // header checksum or footer mismatch
  MM_ERR_CORRUPT_CANARY,   // payload overflowed past its end
  MM_ERR_CORRUPT_PAYLOAD,  // payload contents no longer match their checksum
  MM_ERR_CORRUPT_LINKS,    // block list topology is inconsistent
  MM_ERR_DOUBLE_FREE,      // block was already free
  MM_ERR_NOMEM,            // no block large enough
  MM_ERR_QUARANTINED       // block was isolated after corruption was found
} mm_status_t;

// What the calling thread's most recent allocator call ran into. Reset at the
// start of every call, so it always describes that call and no earlier one.
//
// MM_OK means nothing untoward happened. Any other value is the problem the
// call encountered -- which does not always mean the call failed: an
// allocation that succeeds after stepping over a corrupted block still reports
// MM_ERR_QUARANTINED, because silently discarding that signal is exactly what
// this allocator exists not to do. Use the return value to tell success from
// failure, and this to find out what was seen along the way.
mm_status_t mm_last_error(void);

// Human-readable name for a status code. Never returns NULL.
const char *mm_strerror(mm_status_t status);

// --- Lifecycle -------------------------------------------------------------

// Installs `heap` as the arena. The arena must be at least mm_min_arena()
// bytes and aligned to mm_alignment(). Its previous contents are discarded.
//
// Returns 0 on success, -1 on failure.
int mm_init(void *heap, size_t heap_size);

// Alignment guaranteed for every pointer returned by mm_malloc/mm_realloc.
size_t mm_alignment(void);

// Smallest arena mm_init will accept.
size_t mm_min_arena(void);

// --- Allocation ------------------------------------------------------------

// Allocates `size` bytes. Returns NULL if size is 0 or no block is available.
void *mm_malloc(size_t size);

// Releases a block. Ignores NULL. A pointer this arena did not hand out, or a
// block already free, is reported through mm_last_error() and otherwise
// ignored.
void mm_free(void *ptr);

// Resizes a block, preserving its contents up to the smaller of the two sizes.
// With ptr == NULL this is mm_malloc; with new_size == 0 the block is freed and
// NULL returned. On failure the original block is left untouched.
void *mm_realloc(void *ptr, size_t new_size);

// --- Validated access ------------------------------------------------------

// Copies `len` bytes out of a block, starting `offset` into its payload. The
// block's integrity is verified first.
//
// Returns the number of bytes copied, or -1 on a bad pointer, an
// out-of-bounds range, or detected corruption.
int64_t mm_read(const void *ptr, size_t offset, void *buf, size_t len);

// Copies `len` bytes into a block, starting `offset` into its payload, and
// refreshes the payload checksum. Partial writes are supported at any offset.
//
// Returns the number of bytes written, or -1 on a bad pointer, an
// out-of-bounds range, or detected corruption.
int64_t mm_write(void *ptr, size_t offset, const void *src, size_t len);

// --- Counters --------------------------------------------------------------

// Snapshot of allocator activity. All fields are zero unless the library was
// built with counters enabled, which the benchmark harness uses to report
// utilisation and per-allocation overhead.
typedef struct mm_stats {
  uint64_t alloc_calls;
  uint64_t alloc_failures;
  uint64_t free_calls;
  uint64_t realloc_calls;

  uint64_t live_blocks;
  size_t live_payload_bytes;  // bytes the caller asked for and still holds
  size_t live_block_bytes;    // what those blocks actually occupy

  // Sampled together, at the moment occupancy was highest. Taking them as one
  // snapshot is what makes overhead-per-allocation a meaningful ratio; peak
  // payload and peak occupancy on their own can occur at different times.
  size_t peak_block_bytes;
  size_t peak_payload_bytes;
  uint64_t peak_blocks;

  uint64_t quarantined_blocks;
  size_t quarantined_bytes;
} mm_stats_t;

void mm_stats_get(mm_stats_t *out);
void mm_stats_reset(void);

// --- Integrity -------------------------------------------------------------

// Verifies one block: header checksum, mirrored footer, canary, and payload
// checksum. Returns MM_OK if intact.
mm_status_t mm_verify(const void *ptr);

// Walks every block and checks each one's metadata and the list topology.
// Returns MM_OK if the whole arena is consistent.
mm_status_t mm_check_heap(void);

#ifdef __cplusplus
}
#endif

#endif  // MARS_ALLOCATOR_H_
