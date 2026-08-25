// Allocation counters.
//
// Compiled out entirely unless MM_STATS is defined, so that the benchmark can
// measure the allocator with and without them and show what they cost rather
// than asserting they are free.
//
// They live in the arena rather than in a file-static here, and that is a
// threading decision as much as a tidying one. A counter outside the structure
// the lock protects is a counter the lock does not protect: every one of these
// is a read-modify-write on the allocation path, so a global would be a data
// race on every single call the moment two threads allocate -- and one TSan
// would be right to report. Putting them where the lock already reaches costs
// nothing and needs no atomics.
//
// The alternative was to make each field atomic. That would have been slower
// on the path that matters and would still not have made a *set* of counters
// consistent with each other: peak_block_bytes, peak_payload_bytes and
// peak_blocks are meant to be one snapshot, and three independent atomics are
// three snapshots.

#include "mm_internal.h"

#include <string.h>

#ifdef MM_STATS

// Both of these take the lock. Reading a set of counters that is being updated
// would produce a snapshot in which the peaks and the live figures came from
// different moments, and the ratios computed from them in the benchmark are
// only meaningful because they came from the same one.
// Summed over every arena, one lock at a time.
//
// With more than one arena the three peak figures stop being one snapshot: each
// arena's peak was reached at whatever moment that arena was busiest, and those
// moments are not the same. The sum is therefore an upper bound on the peak the
// process ever actually held, and it is the honest one -- a global peak would
// need a shared counter updated on every allocation, which is the data race
// this whole arrangement exists to avoid. With one arena, which is every
// single-threaded program and every build but MARS_LOCK=arena, it is exact.
void mm_stats_get(mm_stats_t *out) {
  if (out == NULL) return;
  memset(out, 0, sizeof(*out));
  for (mm_arena *a = mm_arena_first(); a != NULL; a = mm_arena_next(a)) {
    mm_guard g = mm_enter_arena(a);
    const mm_stats_t *s = &a->stats;
    out->alloc_calls += s->alloc_calls;
    out->alloc_failures += s->alloc_failures;
    out->free_calls += s->free_calls;
    out->realloc_calls += s->realloc_calls;
    out->live_blocks += s->live_blocks;
    out->live_payload_bytes += s->live_payload_bytes;
    out->live_block_bytes += s->live_block_bytes;
    out->peak_block_bytes += s->peak_block_bytes;
    out->peak_payload_bytes += s->peak_payload_bytes;
    out->peak_blocks += s->peak_blocks;
    out->quarantined_blocks += s->quarantined_blocks;
    out->quarantined_bytes += s->quarantined_bytes;
    out->repaired_blocks += s->repaired_blocks;
    out->repaired_bytes += s->repaired_bytes;
    out->scrub_passes += s->scrub_passes;
    out->scrub_blocks += s->scrub_blocks;
    out->scrub_detections += s->scrub_detections;
    mm_leave_for(g);
  }
}

void mm_stats_reset(void) {
  for (mm_arena *a = mm_arena_first(); a != NULL; a = mm_arena_next(a)) {
    mm_guard g = mm_enter_arena(a);
    memset(&a->stats, 0, sizeof(a->stats));
    mm_leave_for(g);
  }
}

void mm_stats_block_added(size_t block_bytes, size_t payload_bytes) {
  mm_stats_t *s = &g_arena.stats;
  s->live_blocks++;
  s->live_payload_bytes += payload_bytes;
  s->live_block_bytes += block_bytes;
  // Occupancy drives the snapshot: when it reaches a new high, record the
  // payload and block count that went with it.
  if (s->live_block_bytes > s->peak_block_bytes) {
    s->peak_block_bytes = s->live_block_bytes;
    s->peak_payload_bytes = s->live_payload_bytes;
    s->peak_blocks = s->live_blocks;
  }
}

void mm_stats_block_removed(size_t block_bytes, size_t payload_bytes) {
  mm_stats_t *s = &g_arena.stats;
  if (s->live_blocks > 0) s->live_blocks--;
  size_t payload = payload_bytes;
  size_t total = block_bytes;
  s->live_payload_bytes =
      s->live_payload_bytes >= payload ? s->live_payload_bytes - payload : 0;
  s->live_block_bytes =
      s->live_block_bytes >= total ? s->live_block_bytes - total : 0;
}

void mm_stats_note(mm_stats_event_t event, size_t bytes) {
  mm_stats_t *s = &g_arena.stats;
  switch (event) {
    case MM_STAT_ALLOC:        s->alloc_calls++;    break;
    case MM_STAT_ALLOC_FAILED: s->alloc_failures++; break;
    case MM_STAT_FREE:         s->free_calls++;     break;
    case MM_STAT_REALLOC:      s->realloc_calls++;  break;
    case MM_STAT_QUARANTINE:
      s->quarantined_blocks++;
      s->quarantined_bytes += bytes;
      break;
    case MM_STAT_REPAIR:
      s->repaired_blocks++;
      s->repaired_bytes += bytes;
      break;
    case MM_STAT_SCRUB:
      s->scrub_passes++;
      s->scrub_blocks += bytes;  // blocks visited, not bytes
      break;
    case MM_STAT_SCRUB_HIT:
      s->scrub_detections += bytes;
      break;
  }
}

#else

void mm_stats_get(mm_stats_t *out) {
  if (out != NULL) memset(out, 0, sizeof(*out));
}

void mm_stats_reset(void) {}

#endif  // MM_STATS
