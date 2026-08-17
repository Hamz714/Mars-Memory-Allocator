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

const char *mm_profile(void) { return MM_PROFILE_NAME; }

size_t mm_metadata_overhead(void) { return MM_HDR_SIZE + MM_TRAIL; }

// --- Bounds ----------------------------------------------------------------

bool mm_is_block_start(const mm_block *b) {
  if (g_arena.lo == NULL || b == NULL) return false;
  const uint8_t *p = (const uint8_t *)(const void *)b;
  if (p < g_arena.lo || p >= g_arena.hi) return false;
  // Every block starts a whole number of alignment units into the tiling, so
  // an address that does not cannot be one. Checked before the control word is
  // read, because a header that is not there must not be dereferenced.
  if ((size_t)(p - g_arena.lo) % MM_ALIGNMENT != 0) return false;
  return (size_t)(g_arena.hi - p) >= MM_MIN_BLOCK;
}

bool mm_is_block(const mm_block *b) {
  if (!mm_is_block_start(b)) return false;
  size_t n = mm_block_size(b);
  if (n < MM_MIN_BLOCK) return false;
  return n <= (size_t)(g_arena.hi - (const uint8_t *)(const void *)b);
}


size_t mm_max_blocks(void) {
  if (g_arena.lo == NULL) return 0;
  return (size_t)(g_arena.hi - g_arena.lo) / MM_MIN_BLOCK + 1;
}

// --- Validation ------------------------------------------------------------

bool mm_header_ok(const mm_block *b) {
  if (!mm_is_block(b)) return false;
#if MM_HAS_CRC
  return b->hdr_crc == mm_hdr_crc_of(b, mm_block_index(b), g_arena.secret);
#else
  // The fast profile buys its eight-byte header by carrying no checksum at
  // all, so this is the structural check and nothing more. Claiming otherwise
  // here would be the one dishonest line in the allocator.
  return true;
#endif
}

bool mm_extent_corroborated(const mm_block *b) {
  if (!mm_header_ok(b)) return false;
#if MM_HAS_CRC
  // The checksum covers the control word, so it has already established the
  // extent. A free block's boundary tag is then a derivative of a number that
  // has been vouched for: where the two disagree the tag is the damaged copy,
  // and the patrol puts it back rather than giving up on the block.
  return true;
#else
  // No checksum, so nothing has vouched for the control word and mm_header_ok
  // above was a bounds check. An allocated block carries no second copy of its
  // extent under this profile, so there is nothing further to ask of it -- that
  // is the priced cost of an eight-byte header, and saying so is the honest
  // answer rather than refusing every allocated block a walk meets.
  //
  // A free block is different: its last eight bytes repeat the extent, and
  // requiring the two to agree is what separates a block from a run of payload
  // bytes that happens to read like one.
  if (mm_is_used(b)) return true;
  uint64_t tag;
  memcpy(&tag, mm_block_end(b) - sizeof(tag), sizeof(tag));
  return tag == (uint64_t)mm_block_size(b);
#endif
}

bool mm_unchanged(const mm_block *b, size_t block_size, bool used) {
  return mm_header_ok(b) && mm_block_size(b) == block_size &&
         mm_is_used(b) == used;
}

// --- Canary ----------------------------------------------------------------

#if MM_HAS_CANARY

// The canary sits immediately after the payload, at an offset the caller
// chose, so it is not necessarily aligned. Always move it with memcpy.
static uint8_t *canary_at(const mm_block *b) {
  return mm_payload_of(b) + mm_requested_size(b);
}

void mm_write_canary(mm_block *b) {
  uint64_t c = mm_canary_value(mm_block_index(b), g_arena.secret);
  memcpy(canary_at(b), &c, sizeof(c));
}

bool mm_canary_ok(const mm_block *b) {
  // Free blocks carry no canary, and a quarantined block's slack no longer
  // describes a payload, so neither has one to check.
  if (!mm_is_used(b) || mm_is_quarantined(b)) return true;
  uint64_t seen;
  memcpy(&seen, canary_at(b), sizeof(seen));
  return seen == mm_canary_value(mm_block_index(b), g_arena.secret);
}

#else

void mm_write_canary(mm_block *b) { (void)b; }

bool mm_canary_ok(const mm_block *b) {
  (void)b;
  return true;
}

#endif  // MM_HAS_CANARY

// --- Sealing ---------------------------------------------------------------

void mm_seal(mm_block *b) {
  // Under the fast profile there is nothing to seal: the control word is the
  // whole header. The call sites still make it, so that the sequence of
  // operations is the same shape in every profile.
  (void)b;
#if MM_HAS_CRC
  b->hdr_crc = mm_hdr_crc_of(b, mm_block_index(b), g_arena.secret);
#endif
#if MM_HAS_MIRROR
  // The mirror occupies the last MM_MIRROR_SIZE bytes of an allocated block. A
  // free block spends those bytes on its boundary tag instead, and needs no
  // mirror: the tag already records its extent.
  if (mm_is_used(b)) {
    memcpy(mm_block_end(b) - MM_MIRROR_SIZE, b, MM_HDR_SIZE);
  }
#endif
}

// Seals `b` and brings the block after it back into agreement, without
// invoking recovery -- this is what recovery itself uses, where recursing
// would be neither necessary nor safe.
static void seal_and_link(mm_block *b) {
  if (!mm_is_used(b)) mm_write_free_footer(b);
  mm_seal(b);

  uint8_t *end = mm_block_end(b);
  if (end >= g_arena.hi) return;
  mm_block *n = (mm_block *)(void *)end;
  // A neighbour that fails validation is never written through: its control
  // word decides where its own trailer lands, so sealing a corrupted one
  // scatters writes across the arena.
  if (!mm_header_ok(n)) return;
  if (mm_prev_in_use(n) != mm_is_used(b)) {
    mm_set_prev_in_use(n, mm_is_used(b));
    mm_seal(n);
  }
}

bool mm_publish(mm_block *b) {
  // Everything below is positioned by this block's own control word: the
  // boundary tag lands at its end, and the step to its successor is its whole
  // extent. So an extent that is not inside the arena must never be written
  // through, whatever put it there. This is deliberately a check at the write
  // rather than only on the paths that reach it -- the promise never to read or
  // write outside the arena is the one thing every profile guarantees
  // unconditionally, and a guarantee that holds only because every caller is
  // currently correct is not unconditional.
  //
  // Refusing on its own would leave the tiling broken rather than wildly
  // written, which is not an improvement, so the span goes the way every lost
  // extent goes: reported, and surrendered as a quarantined block.
  if (!mm_is_block(b)) {
    mm_fail(MM_ERR_CORRUPT_HEADER);
    (void)mm_rescue(b);
    return false;
  }

  if (!mm_is_used(b)) mm_write_free_footer(b);
  mm_seal(b);

  mm_block *n = mm_recover_next(b);
  if (n == NULL) return true;
  if (mm_prev_in_use(n) != mm_is_used(b)) {
    mm_set_prev_in_use(n, mm_is_used(b));
    mm_seal(n);
  }
  return true;
}

// --- Stepping backwards ----------------------------------------------------

mm_block *mm_prev_free_block(const mm_block *b) {
  // The boundary tag is legible only when PREV_IN_USE is clear. Read it in any
  // other circumstance and it is payload bytes, or the tail of a mirror.
  if (mm_prev_in_use(b)) return NULL;

  const uint8_t *p = (const uint8_t *)(const void *)b;
  if ((size_t)(p - g_arena.lo) < MM_MIN_BLOCK) return NULL;

  uint64_t n = mm_read_prev_footer(b);
  if (n < MM_MIN_BLOCK || n % MM_ALIGNMENT != 0) return NULL;
  if (n > (uint64_t)(size_t)(p - g_arena.lo)) return NULL;

  mm_block *prev = (mm_block *)(void *)(p - (size_t)n);
  // Corroborate the tag against the header it claims to belong to, rather than
  // trusting either one on its own.
  if (!mm_header_ok(prev)) return NULL;
  if (mm_block_size(prev) != (size_t)n) return NULL;
  if (mm_is_used(prev)) return NULL;
  return prev;
}

// --- Recovery primitives ---------------------------------------------------

// How far the resynchronisation scan will walk, in MM_ALIGNMENT steps.
// Recovery runs only on the damage path, but it must still terminate promptly
// on a large arena. The cost is that damage spanning more than this many steps
// surrenders the rest of the arena rather than stepping over it.
#define MM_RECOVERY_STEPS 65536u

// Blocks tile the arena exactly, so whatever sits after a block of this extent
// must be either the end of the arena or another intact block. That tiling is
// the structural redundancy which makes a recovered extent trustworthy rather
// than merely plausible.
static bool tiling_agrees(const mm_block *b) {
  uint8_t *end = mm_block_end(b);
  if (end == g_arena.hi) return true;
  if ((size_t)(g_arena.hi - end) < MM_MIN_BLOCK) return false;
  return mm_extent_corroborated((const mm_block *)(const void *)end);
}

// The next address at or after `start + MM_MIN_BLOCK` carrying a header that
// stands up on its own and tiles with what follows it. NULL if there is none:
// a real block is at least MM_MIN_BLOCK long, so nothing nearer can be one.
//
// Both halves ask mm_extent_corroborated rather than mm_header_ok, because this
// is a *search*: the answer is not reached by stepping over a block already
// trusted, so the only thing standing between it and a run of payload bytes is
// how much the candidate can be made to corroborate itself. Accepting a
// candidate that is not a block boundary does not merely surrender the wrong
// span -- it leaves the tiling permanently out of step, and every later walk
// then steps through blocks that are not there.
static uint8_t *scan_resync(uint8_t *start) {
  size_t steps = 0;
  for (uint8_t *p = start + MM_MIN_BLOCK;
       p < g_arena.hi && (size_t)(g_arena.hi - p) >= MM_MIN_BLOCK &&
       steps < MM_RECOVERY_STEPS;
       p += MM_ALIGNMENT, steps++) {
    const mm_block *c = (const mm_block *)(const void *)p;
    if (mm_extent_corroborated(c) && tiling_agrees(c)) return p;
  }
  return NULL;
}

// Turns [start, start + span) into a block that exists only so the arena keeps
// tiling: permanently allocated, never merged, never handed out again.
static mm_block *abandon(uint8_t *start, size_t span, bool prev_used) {
  if (span < MM_MIN_BLOCK) {
    mm_fail(MM_ERR_CORRUPT_LINKS);
    return NULL;
  }

  mm_block *q = (mm_block *)(void *)start;
  q->word = 0;
#if MM_HAS_CRC
  q->payload_crc = 0;
#endif
  mm_set_word(q, span, 0, true, true);
  mm_set_prev_in_use(q, prev_used);
  // Whatever block boundaries used to be inside this span are gone with it.
  mm_scrub_forget(start, start + span);
  seal_and_link(q);

  g_arena.lost_bytes += span;
  MM_STAT_NOTE(MM_STAT_QUARANTINE, span);
  mm_fail(MM_ERR_CORRUPT_LINKS);
  return q;
}

#if MM_HAS_MIRROR
// Reads the tail mirror that would belong to a block ending at `resync` and
// says whether it stands up. Nothing is written: the caller decides what to do
// with the answer.
//
// This is where the paranoid profile earns its extra sixteen bytes. A mirror's
// position is fixed relative to the *end* of its block, and the end is exactly
// what the resynchronisation scan has just found -- so once the scan has a
// foothold the repair is one bounded check, not the search over every possible
// extent that a header-position mirror would need.
static bool mirror_says(const uint8_t *lo_bound, const uint8_t *resync,
                        mm_block *out, uint8_t **out_start) {
  memcpy(out, resync - MM_MIRROR_SIZE, MM_HDR_SIZE);

  size_t n = mm_block_size(out);
  if (n < MM_MIN_BLOCK || n > (size_t)(resync - lo_bound)) return false;
  if (!mm_is_used(out)) return false;  // a free block keeps a tag, not a mirror

  uint8_t *start = (uint8_t *)(uintptr_t)(const void *)(resync - n);
  if ((size_t)(start - g_arena.lo) % MM_ALIGNMENT != 0) return false;

  // The mirror carries its own checksum, and that checksum is bound to the
  // block's index within the arena. A mirror lifted from anywhere else
  // therefore fails here, which is what makes a false repair vanishingly
  // unlikely rather than merely unlikely.
  uint64_t index = (uint64_t)((size_t)(start - g_arena.lo) / MM_ALIGNMENT);
  if (out->hdr_crc != mm_hdr_crc_of(out, index, g_arena.secret)) return false;

  *out_start = start;
  return true;
}
#endif  // MM_HAS_MIRROR

// --- Quarantine and rescue -------------------------------------------------

void mm_quarantine(mm_block *b) {
  // Only the address is required here. A block whose start is known and whose
  // recorded extent is impossible is exactly what recovery below is for, and
  // turning it away would leave the damage in place unreported.
  if (!mm_is_block_start(b)) return;
  if (!mm_header_ok(b)) {
    // The extent cannot be trusted, so neither can the address of anything
    // this block owns. Surrender the span instead of writing through it.
    (void)mm_rescue(b);
    return;
  }
  if (mm_is_quarantined(b)) {
    mm_fail(MM_ERR_QUARANTINED);
    return;
  }

  size_t n = mm_block_size(b);
  bool was_free = !mm_is_used(b);
  // A free block is in a bin, and has to come out of it before its interior
  // stops meaning what that bin thinks it means.
  if (was_free) {
    mm_bin_remove(b);
    // Taking a block out of a bin writes memory, so the extent read above is
    // no longer known to be this block's. See mm_unchanged.
    if (!mm_unchanged(b, n, false)) {
      mm_fail(MM_ERR_CORRUPT_LINKS);
      (void)mm_rescue(b);
      return;
    }
  }

  mm_set_word(b, n, 0, true, true);
#if MM_HAS_CRC
  b->payload_crc = 0;
#endif
  if (!mm_publish(b)) return;  // reported, and the span already surrendered

  g_arena.lost_bytes += n;
  MM_STAT_NOTE(MM_STAT_QUARANTINE, n);
  mm_fail(MM_ERR_QUARANTINED);
}

bool mm_rescue(mm_block *b) {
  if (mm_header_ok(b)) return true;
  // The start is all that is needed: the extent is the thing being recovered,
  // so requiring it to be legible first would refuse every block this exists
  // to deal with.
  if (!mm_is_block_start(b)) return false;

  uint8_t *start = (uint8_t *)(void *)b;
  uint8_t *resync = scan_resync(start);
  bool prev_used = mm_prev_in_use(b);

#if MM_HAS_MIRROR
  if (resync != NULL) {
    mm_block rebuilt;
    uint8_t *at = NULL;
    if (mirror_says(start, resync, &rebuilt, &at) && at == start) {
      memcpy(b, &rebuilt, MM_HDR_SIZE);
      mm_set_prev_in_use(b, prev_used);
      seal_and_link(b);
      MM_STAT_NOTE(MM_STAT_REPAIR, mm_block_size(b));
      mm_fail(MM_ERR_CORRUPT_HEADER);
      return true;
    }
  }
#endif

  size_t span = resync != NULL ? (size_t)(resync - start)
                               : (size_t)(g_arena.hi - start);
  (void)abandon(start, span, prev_used);
  return false;
}

// --- Walking over damage ---------------------------------------------------

mm_block *mm_recover_next(mm_block *owner) {
  uint8_t *start = mm_block_end(owner);
  if (start >= g_arena.hi) return NULL;
  if ((size_t)(g_arena.hi - start) < MM_MIN_BLOCK) return NULL;

  mm_block *cand = (mm_block *)(void *)start;
  if (mm_header_ok(cand)) return cand;

  // The header is unusable, so the extent it recorded is gone and stepping
  // forward is no longer possible from here. Find the next header that stands
  // up on its own; that is the only way back in step.
  uint8_t *resync = scan_resync(start);
  bool owner_used = mm_is_used(owner);

#if MM_HAS_MIRROR
  if (resync != NULL) {
    mm_block rebuilt;
    uint8_t *at = NULL;
    if (mirror_says(start, resync, &rebuilt, &at)) {
      if (at == start) {
        memcpy(cand, &rebuilt, MM_HDR_SIZE);
        mm_set_prev_in_use(cand, owner_used);
        seal_and_link(cand);
        MM_STAT_NOTE(MM_STAT_REPAIR, mm_block_size(cand));
        mm_fail(MM_ERR_CORRUPT_HEADER);
        return cand;
      }
      // The mirror belongs to a block further along, so whatever lies between
      // the owner and it was damaged past rebuilding. Give up that span alone
      // and keep the block the mirror does describe.
      if ((size_t)(at - start) >= MM_MIN_BLOCK) {
        mm_block *r = (mm_block *)(void *)at;
        memcpy(r, &rebuilt, MM_HDR_SIZE);
        mm_set_prev_in_use(r, true);  // the abandoned span counts as allocated
        seal_and_link(r);
        MM_STAT_NOTE(MM_STAT_REPAIR, mm_block_size(r));
        return abandon(start, (size_t)(at - start), owner_used);
      }
    }
  }
#endif

  size_t span = resync != NULL ? (size_t)(resync - start)
                               : (size_t)(g_arena.hi - start);
  return abandon(start, span, owner_used);
}

// --- The patrol ------------------------------------------------------------
//
// O(1) allocation means the allocator stops touching most of the arena. A
// block that is neither allocated, freed, nor next to something being
// coalesced is never looked at again, so damage to it would sit there
// indefinitely -- and the linear search this brief removed used to find that
// damage incidentally, on every single call.
//
// Two tiers replace it. Validate-on-touch stays where it was: every block
// popped by malloc, every block freed, and both coalescing neighbours. On top
// of that this bounded patrol walks the implicit list a few blocks at a time,
// resuming where it stopped, so that cold memory is still covered -- at a cost
// that is a constant per operation rather than a function of arena size. It is
// what a hardware ECC scrubber does, and the trade it makes (detection latency
// against throughput) is measurable rather than assumed: the fault injector
// records the latency, and tools/faultinject takes --scrub-interval so the
// curve can be swept.

#define MM_SCRUB_INTERVAL_DEFAULT ((size_t)1024)
#define MM_SCRUB_BUDGET_DEFAULT ((size_t)16)

static size_t g_scrub_interval = MM_SCRUB_INTERVAL_DEFAULT;
static size_t g_scrub_budget = MM_SCRUB_BUDGET_DEFAULT;
static size_t g_ops_since_scrub;

void mm_set_scrub_interval(size_t ops, size_t budget_blocks) {
  g_scrub_interval = ops;
  g_scrub_budget = budget_blocks == 0 ? MM_SCRUB_BUDGET_DEFAULT : budget_blocks;
  g_ops_since_scrub = 0;
}

void mm_scrub_forget(uint8_t *from, uint8_t *to) {
  if (g_arena.scrub_at > from && g_arena.scrub_at < to) g_arena.scrub_at = from;
}

// Checks one block and reports the first thing wrong with it, or MM_OK.
// Repairs what is safely derivable and quarantines what is not; the block is
// left walkable either way, because the walk has to carry on past it.
static mm_status_t scrub_block(mm_block *b) {
  if (!mm_canary_ok(b)) {
    // Someone overran their payload. The same response as on any other path
    // that meets a broken canary: the block is given up on.
    mm_quarantine(b);
    return MM_ERR_CORRUPT_CANARY;
  }

  if (!mm_is_used(b)) {
    uint64_t tag;
    memcpy(&tag, mm_block_end(b) - sizeof(tag), sizeof(tag));
    if (tag == (uint64_t)mm_block_size(b)) return MM_OK;
#if MM_HAS_CRC
    // The boundary tag is a pure derivative of an extent the checksum has
    // already vouched for, so putting it back is a repair rather than a guess.
    // Left alone it would break the next backward coalesce.
    mm_write_free_footer(b);
    MM_STAT_NOTE(MM_STAT_REPAIR, mm_block_size(b));
#else
    // Under `fast` there is no checksum, so the header has not been vouched for
    // either: the two copies of the extent disagree and nothing can say which
    // one is wrong. Rewriting the tag from the header would not be a repair, it
    // would be *manufacturing* the corroboration that mm_extent_corroborated
    // relies on -- and a walk that later writes into this block on the strength
    // of it is the wild write this whole check exists to prevent.
    //
    // So it is reported and left as it is. The block stays in its bin and stays
    // allocatable; what it loses is being steppable backwards over, which
    // mm_prev_free_block already refuses on exactly this evidence.
#endif
    return MM_ERR_CORRUPT_LINKS;
  }

#if MM_HAS_CRC
  if (b->payload_crc != 0 &&
      mm_payload_crc(mm_payload_of(b), mm_requested_size(b)) != b->payload_crc) {
    // Reported, deliberately not quarantined. mm_read quarantines here because
    // the caller asked for the bytes and must not be handed wrong ones; the
    // patrol was not asked for anything. A payload checksum only means
    // something if every write went through mm_write, and a caller who wrote
    // through their own pointer -- which the API permits -- would otherwise
    // have a live block destroyed behind their back by a passing patrol. So
    // this says what it saw and leaves the block to its owner.
    return MM_ERR_CORRUPT_PAYLOAD;
  }
#endif
  return MM_OK;
}

// The patrol proper. Does not clear the thread status: it runs underneath an
// ordinary allocator call, and what it finds has to survive to mm_last_error()
// alongside whatever that call ran into.
static mm_status_t scrub_run(size_t budget_blocks) {
  if (budget_blocks == 0) return MM_OK;

  uint8_t *p = g_arena.scrub_at;
  if (p == NULL || p < g_arena.lo || p >= g_arena.hi) p = g_arena.lo;

  mm_status_t found = MM_OK;
  size_t visited = 0;
  size_t hits = 0;
  // Bounds the loop whatever the arena turns out to look like, including the
  // case where the budget exceeds the number of blocks and the walk laps.
  size_t guard = mm_max_blocks();

  while (visited < budget_blocks && guard-- > 0) {
    if (p >= g_arena.hi) p = g_arena.lo;
    mm_block *b = (mm_block *)(void *)p;

    if (!mm_header_ok(b)) {
      // The extent this block recorded is exactly what the damage destroyed,
      // so there is no stepping past it. Recovery puts it back where the
      // profile carries a mirror and surrenders its span where it does not;
      // either way the arena still tiles afterwards.
      if (found == MM_OK) found = MM_ERR_CORRUPT_HEADER;
      hits++;
      (void)mm_rescue(b);
      if (!mm_header_ok(b)) {
        p = g_arena.lo;  // nothing legible here; start the lap again
        continue;
      }
    }

    mm_status_t s = scrub_block(b);
    if (s != MM_OK) {
      if (found == MM_OK) found = s;
      hits++;
    }

    visited++;
    p += mm_block_size(b);
  }

  g_arena.scrub_at = p;
  MM_STAT_NOTE(MM_STAT_SCRUB, visited);
  if (hits > 0) MM_STAT_NOTE(MM_STAT_SCRUB_HIT, hits);
  if (found != MM_OK) return mm_fail(found);
  return MM_OK;
}

void mm_scrub_tick(void) {
  if (g_scrub_interval == 0 || g_arena.base == NULL) return;
  if (++g_ops_since_scrub < g_scrub_interval) return;
  g_ops_since_scrub = 0;
  (void)scrub_run(g_scrub_budget);
}

mm_status_t mm_scrub(size_t budget_blocks) {
  mm_clear_error();
  if (g_arena.base == NULL) return mm_fail(MM_ERR_NOT_INITIALIZED);
  return scrub_run(budget_blocks);
}

// --- Pointer recovery ------------------------------------------------------

mm_block *mm_block_of(const void *ptr) {
  if (g_arena.base == NULL) {
    mm_fail(MM_ERR_NOT_INITIALIZED);
    return NULL;
  }
  // Range-check the payload pointer BEFORE reading anything near it: the
  // header lies just below it, and a foreign pointer must never be
  // dereferenced.
  const uint8_t *p = (const uint8_t *)ptr;
  if (p == NULL || p < g_arena.lo + MM_HDR_SIZE || p >= g_arena.hi) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  if ((size_t)(p - (g_arena.lo + MM_HDR_SIZE)) % MM_ALIGNMENT != 0) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }

  // There is no clue byte to cross-check any more. The header size is a
  // compile-time constant and every payload is 16-aligned by construction, so
  // the header is exactly here or the pointer was never ours -- and the
  // checksum, bound to this block's index, decides which.
  mm_block *b = (mm_block *)(void *)(p - MM_HDR_SIZE);
  if (!mm_is_block(b)) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  return b;
}

// --- Public checks ---------------------------------------------------------

mm_status_t mm_verify(const void *ptr) {
  mm_clear_error();
  mm_block *b = mm_block_of(ptr);
  if (b == NULL) return mm_last_error();

  if (!mm_header_ok(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
  if (mm_is_quarantined(b)) return mm_fail(MM_ERR_QUARANTINED);
  if (!mm_canary_ok(b)) return mm_fail(MM_ERR_CORRUPT_CANARY);

#if MM_HAS_CRC
  if (mm_is_used(b) && b->payload_crc != 0) {
    uint32_t got = mm_payload_crc(mm_payload_of(b), mm_requested_size(b));
    if (got != b->payload_crc) return mm_fail(MM_ERR_CORRUPT_PAYLOAD);
  }
#endif
  return MM_OK;
}

mm_status_t mm_check_heap(void) {
  mm_clear_error();
  if (g_arena.base == NULL) return mm_fail(MM_ERR_NOT_INITIALIZED);

  size_t budget = mm_max_blocks();
  size_t covered = 0;
  size_t lost = 0;
  bool have_prev = false;
  bool prev_free = false;
  // What the tiling says each bin should hold. The bins are checked against
  // this afterwards, never the other way round.
  size_t per_bin[MM_BIN_COUNT] = {0};

  // The implicit list is the walk: step block by block through the tiling
  // rather than following stored links. The links are cross-checked against
  // this afterwards, not the other way round -- geometry is the thing that
  // cannot quietly disagree with itself.
  for (uint8_t *p = g_arena.lo; p < g_arena.hi;) {
    if (budget-- == 0) return mm_fail(MM_ERR_CORRUPT_LINKS);

    mm_block *b = (mm_block *)(void *)p;
    if (!mm_is_block(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
    if (!mm_header_ok(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);

    // PREV_IN_USE has to agree with what actually precedes this block. The
    // first block has nothing before it, and reports its predecessor as in use
    // so that nobody tries to step backwards off the front.
    bool expect_prev_used = have_prev ? !prev_free : true;
    if (mm_prev_in_use(b) != expect_prev_used) {
      return mm_fail(MM_ERR_CORRUPT_LINKS);
    }
    if (!mm_canary_ok(b)) return mm_fail(MM_ERR_CORRUPT_CANARY);

    size_t n = mm_block_size(b);
    if (mm_is_used(b)) {
      if (mm_is_quarantined(b)) lost += n;
      prev_free = false;
    } else {
      // The boundary tag must repeat the extent, or stepping backwards over
      // this block would land somewhere else entirely.
      uint64_t tag;
      memcpy(&tag, p + n - sizeof(tag), sizeof(tag));
      if (tag != (uint64_t)n) return mm_fail(MM_ERR_CORRUPT_LINKS);
      // Two adjacent free blocks mean a coalesce was missed.
      if (prev_free) return mm_fail(MM_ERR_CORRUPT_LINKS);
      per_bin[mm_bin_of(n)]++;
      prev_free = true;
    }

    covered += n;
    have_prev = true;
    p += n;
  }

  // Blocks tile the arena exactly -- quarantined space included, since giving
  // up on a block leaves it in the tiling rather than taking it out. Anything
  // else means space has gone missing rather than been surrendered on purpose.
  if (covered != (size_t)(g_arena.hi - g_arena.lo)) {
    return mm_fail(MM_ERR_CORRUPT_LINKS);
  }
  if (lost != g_arena.lost_bytes) return mm_fail(MM_ERR_CORRUPT_LINKS);

  // Now the bins, checked against what the tiling just said: every free block
  // in exactly one bin, in the bin its size asks for, no allocated block in
  // any of them, and a bitmap that agrees about which are empty.
  return mm_bins_check(per_bin);
}
