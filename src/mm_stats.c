// Allocation counters.
//
// Compiled out entirely unless MM_STATS is defined, so that the benchmark can
// measure the allocator with and without them and show what they cost rather
// than asserting they are free.

#include "mm_internal.h"

#include <string.h>

#ifdef MM_STATS

static mm_stats_t g_stats;

void mm_stats_get(mm_stats_t *out) {
  if (out != NULL) *out = g_stats;
}

void mm_stats_reset(void) { memset(&g_stats, 0, sizeof(g_stats)); }

void mm_stats_block_added(const mm_header *block) {
  g_stats.live_blocks++;
  g_stats.live_payload_bytes += block->requested_size;
  g_stats.live_block_bytes += MM_BLOCK_TOTAL(block->size);
  // Occupancy drives the snapshot: when it reaches a new high, record the
  // payload and block count that went with it.
  if (g_stats.live_block_bytes > g_stats.peak_block_bytes) {
    g_stats.peak_block_bytes = g_stats.live_block_bytes;
    g_stats.peak_payload_bytes = g_stats.live_payload_bytes;
    g_stats.peak_blocks = g_stats.live_blocks;
  }
}

void mm_stats_block_removed(const mm_header *block) {
  if (g_stats.live_blocks > 0) g_stats.live_blocks--;
  size_t payload = block->requested_size;
  size_t total = MM_BLOCK_TOTAL(block->size);
  g_stats.live_payload_bytes =
      g_stats.live_payload_bytes >= payload
          ? g_stats.live_payload_bytes - payload : 0;
  g_stats.live_block_bytes =
      g_stats.live_block_bytes >= total ? g_stats.live_block_bytes - total : 0;
}

void mm_stats_note(mm_stats_event_t event, size_t bytes) {
  switch (event) {
    case MM_STAT_ALLOC:        g_stats.alloc_calls++;   break;
    case MM_STAT_ALLOC_FAILED: g_stats.alloc_failures++; break;
    case MM_STAT_FREE:         g_stats.free_calls++;    break;
    case MM_STAT_REALLOC:      g_stats.realloc_calls++; break;
    case MM_STAT_QUARANTINE:
      g_stats.quarantined_blocks++;
      g_stats.quarantined_bytes += bytes;
      break;
  }
}

#else

void mm_stats_get(mm_stats_t *out) {
  if (out != NULL) memset(out, 0, sizeof(*out));
}

void mm_stats_reset(void) {}

#endif  // MM_STATS
