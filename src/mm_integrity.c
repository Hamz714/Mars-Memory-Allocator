// Checksums, canaries, block recovery, and heap-wide consistency checking.

#include "mm_internal.h"

#include <string.h>

#include "mm_arena.h"

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
    case MM_ERR_DEGRADED:        return "intact; payload unchecked in this mode";
  }
  return "unknown status";
}

const char *mm_profile(void) { return MM_PROFILE_NAME; }

// --- Modes -----------------------------------------------------------------

mm_mode_t mm_get_mode(void) { return g_arena.mode; }

// Whether a payload checksum is being kept up to date at all. Two things have
// to hold: the profile must carry the field, and the mode must be one where
// every write goes through the allocator. Under MM_MODE_LIBC the second fails
// by construction -- that is what the mode means -- so nothing establishes a
// payload checksum and nothing has one to check.
bool mm_payload_crc_live(void) {
#if MM_HAS_CRC
  return g_arena.mode == MM_MODE_MANAGED;
#else
  return false;
#endif
}

#if MM_HAS_CRC
// Clears every payload checksum in the arena and re-seals the headers over
// them. Called when the mode stops maintaining them: a checksum that is no
// longer refreshed is not a weaker detector but a false one, and it would
// report the first legitimate store through a raw pointer as corruption.
static void forget_payload_crcs(void) {
  for (const mm_span *s = g_arena.spans; s != NULL; s = s->next) {
    size_t budget = mm_span_max_blocks(s);
    for (uint8_t *p = s->lo; p < s->hi && budget-- > 0;) {
      mm_block *b = (mm_block *)(void *)p;
      // Damage is not this function's business, and a block it cannot read is
      // where the walk stops: there is no way past an illegible extent.
      if (!mm_header_ok(b)) break;
      if (b->payload_crc != 0) {
        b->payload_crc = 0;
        mm_seal(b);
      }
      p += mm_block_size(b);
    }
  }
}
#endif

void mm_set_mode(mm_mode_t mode) {
  if (mode != MM_MODE_MANAGED && mode != MM_MODE_LIBC) return;
  if (g_arena.mode == mode) return;
  g_arena.mode = mode;
#if MM_HAS_CRC
  if (mode == MM_MODE_LIBC && mm_arena_live()) forget_payload_crcs();
#endif
}

size_t mm_metadata_overhead(void) { return MM_HDR_SIZE + MM_TRAIL; }

// --- Bounds ----------------------------------------------------------------

// The two structural questions, asked of a span that has already been found.
//
// Split out because `mm_header_ok` is the hottest thing in the allocator --
// every validation, every walk and every coalesce goes through it -- and
// written the obvious way it asked which span a block was in *three* times
// over: once through mm_is_block_start, again through mm_is_block, and again
// through mm_block_index for the checksum. One lookup answers all three.
//
// No throughput claim is attached to that, because none survived measurement.
// An interleaved A/B against the previous layout on this machine moved between
// -14% and +41% by workload while glibc's own figures moved by 2x in the same
// run, which is the machine and not the code. It is here because doing the
// same work three times is worse than doing it once, which needs no benchmark.
static bool start_ok_in(const mm_span *s, const mm_block *b) {
  const uint8_t *p = (const uint8_t *)(const void *)b;
  // Every block starts a whole number of alignment units into the tiling, so
  // an address that does not cannot be one. Checked before the control word is
  // read, because a header that is not there must not be dereferenced.
  if ((size_t)(p - s->lo) % MM_ALIGNMENT != 0) return false;
  return (size_t)(s->hi - p) >= MM_MIN_BLOCK;
}

static bool block_ok_in(const mm_span *s, const mm_block *b) {
  if (!start_ok_in(s, b)) return false;
  size_t n = mm_block_size(b);
  if (n < MM_MIN_BLOCK) return false;
  return n <= (size_t)(s->hi - (const uint8_t *)(const void *)b);
}

bool mm_is_block_start(const mm_block *b) {
  if (b == NULL) return false;
  // Which span, before anything else. An address in none of them was never
  // ours, and the bounds every question here is asked against are that span's
  // rather than the whole arena's. Blocks never straddle a span, so this is
  // the entirety of what "inside the arena" means for a block.
  const mm_span *s = mm_span_of(b);
  return s != NULL && start_ok_in(s, b);
}

bool mm_is_block(const mm_block *b) {
  if (b == NULL) return false;
  const mm_span *s = mm_span_of(b);
  return s != NULL && block_ok_in(s, b);
}

size_t mm_span_max_blocks(const mm_span *s) {
  return (size_t)(s->hi - s->lo) / MM_MIN_BLOCK + 1;
}

size_t mm_max_blocks(void) {
  return g_arena.total_bytes / MM_MIN_BLOCK + 1;
}

// --- Validation ------------------------------------------------------------

bool mm_header_ok(const mm_block *b) {
  if (b == NULL) return false;
  // One span lookup, and everything below is asked of it: the structure, the
  // extent, and the index the checksum is bound to. See start_ok_in.
  const mm_span *s = mm_span_of(b);
  if (s == NULL || !block_ok_in(s, b)) return false;
#if MM_HAS_CRC
  return b->hdr_crc == mm_hdr_crc_of(b, mm_index_in(s, b), g_arena.secret);
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

  const mm_span *s = mm_span_of(b);
  if (s == NULL) return;
  uint8_t *end = mm_block_end(b);
  if (end >= s->hi) return;
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

  const mm_span *s = mm_span_of(b);
  if (s == NULL) return NULL;
  const uint8_t *p = (const uint8_t *)(const void *)b;
  // Bounded by this span's start rather than the arena's: what lies below a
  // chunk belongs to somebody else entirely. The first block of every span
  // reports its predecessor as in use, which is what stops this being reached
  // there at all.
  if ((size_t)(p - s->lo) < MM_MIN_BLOCK) return NULL;

  uint64_t n = mm_read_prev_footer(b);
  if (n < MM_MIN_BLOCK || n % MM_ALIGNMENT != 0) return NULL;
  if (n > (uint64_t)(size_t)(p - s->lo)) return NULL;

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
static bool tiling_agrees(const mm_span *s, const mm_block *b) {
  uint8_t *end = mm_block_end(b);
  if (end == s->hi) return true;
  if ((size_t)(s->hi - end) < MM_MIN_BLOCK) return false;
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
static uint8_t *scan_resync(const mm_span *s, uint8_t *start) {
  size_t steps = 0;
  for (uint8_t *p = start + MM_MIN_BLOCK;
       p < s->hi && (size_t)(s->hi - p) >= MM_MIN_BLOCK &&
       steps < MM_RECOVERY_STEPS;
       p += MM_ALIGNMENT, steps++) {
    const mm_block *c = (const mm_block *)(const void *)p;
    if (mm_extent_corroborated(c) && tiling_agrees(s, c)) return p;
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
static bool mirror_says(const mm_span *s, const uint8_t *lo_bound,
                        const uint8_t *resync, mm_block *out,
                        uint8_t **out_start) {
  memcpy(out, resync - MM_MIRROR_SIZE, MM_HDR_SIZE);

  size_t n = mm_block_size(out);
  if (n < MM_MIN_BLOCK || n > (size_t)(resync - lo_bound)) return false;
  if (!mm_is_used(out)) return false;  // a free block keeps a tag, not a mirror

  uint8_t *start = (uint8_t *)(uintptr_t)(const void *)(resync - n);
  if ((size_t)(start - s->lo) % MM_ALIGNMENT != 0) return false;

  // The mirror carries its own checksum, and that checksum is bound to the
  // block's index within the arena. A mirror lifted from anywhere else
  // therefore fails here, which is what makes a false repair vanishingly
  // unlikely rather than merely unlikely.
  uint64_t index = mm_index_in(s, (const mm_block *)(const void *)start);
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
  // The start is all that is needed *of the block*: the extent is the thing
  // being recovered, so requiring it to be legible first would refuse every
  // block this exists to deal with. That the start is a real one is the
  // caller's to vouch for -- see the declaration, and scrub_run for the one
  // caller that cannot always do so.
  if (!mm_is_block_start(b)) return false;

  const mm_span *s = mm_span_of(b);
  uint8_t *start = (uint8_t *)(void *)b;
  uint8_t *resync = scan_resync(s, start);
  bool prev_used = mm_prev_in_use(b);

#if MM_HAS_MIRROR
  if (resync != NULL) {
    mm_block rebuilt;
    uint8_t *at = NULL;
    if (mirror_says(s, start, resync, &rebuilt, &at) && at == start) {
      memcpy(b, &rebuilt, MM_HDR_SIZE);
      mm_set_prev_in_use(b, prev_used);
      seal_and_link(b);
      MM_STAT_NOTE(MM_STAT_REPAIR, mm_block_size(b));
      mm_fail(MM_ERR_CORRUPT_HEADER);
      return true;
    }
  }
#endif

  // Whatever cannot be resynchronised is surrendered as far as the end of this
  // span -- never further. The next mapping is somebody else's memory, and the
  // one promise every profile makes unconditionally is not to write into it.
  size_t span = resync != NULL ? (size_t)(resync - start)
                               : (size_t)(s->hi - start);
  (void)abandon(start, span, prev_used);
  return false;
}

// --- Walking over damage ---------------------------------------------------

mm_block *mm_recover_next(mm_block *owner) {
  // The owner's span, not the block after it: `start` may be exactly the end
  // of the span, which belongs to no span at all.
  const mm_span *s = mm_span_of(owner);
  if (s == NULL) return NULL;
  uint8_t *start = mm_block_end(owner);
  if (start >= s->hi) return NULL;
  if ((size_t)(s->hi - start) < MM_MIN_BLOCK) return NULL;

  mm_block *cand = (mm_block *)(void *)start;
  if (mm_header_ok(cand)) return cand;

  // The header is unusable, so the extent it recorded is gone and stepping
  // forward is no longer possible from here. Find the next header that stands
  // up on its own; that is the only way back in step.
  uint8_t *resync = scan_resync(s, start);
  bool owner_used = mm_is_used(owner);

#if MM_HAS_MIRROR
  if (resync != NULL) {
    mm_block rebuilt;
    uint8_t *at = NULL;
    if (mirror_says(s, start, resync, &rebuilt, &at)) {
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
                               : (size_t)(s->hi - start);
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

void mm_scrub_leave(const mm_span *s) {
  if (g_arena.scrub_span != s) return;
  // Not "move to the next span": this is called while the span is being taken
  // out of the list, so there is no next to speak of yet. The patrol starts its
  // lap again, which costs it one pass and cannot dangle.
  g_arena.scrub_span = NULL;
  g_arena.scrub_at = NULL;
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

  mm_span *s = g_arena.scrub_span;
  if (s == NULL) s = g_arena.spans;
  if (s == NULL) return MM_OK;

  uint8_t *p = g_arena.scrub_at;
  if (p == NULL || p < s->lo || p >= s->hi) p = s->lo;

  mm_status_t found = MM_OK;
  size_t visited = 0;
  size_t hits = 0;
  // Bounds the loop whatever the arena turns out to look like, including the
  // case where the budget exceeds the number of blocks and the walk laps. The
  // span count is added because moving between spans consumes an iteration
  // without visiting a block.
  size_t guard = mm_max_blocks() + g_arena.span_count;

  while (visited < budget_blocks && guard-- > 0) {
    // Off the end of this span, so on to the next -- and round to the first
    // again after the last, because the patrol is a lap and not a sweep.
    if (p >= s->hi) {
      s = s->next != NULL ? s->next : g_arena.spans;
      p = s->lo;
    }
    mm_block *b = (mm_block *)(void *)p;

    if (!mm_header_ok(b)) {
      if (found == MM_OK) found = MM_ERR_CORRUPT_HEADER;
      hits++;
      // Recovery needs a block start the caller vouches for, and the patrol's
      // cursor is not one. It is wherever the last extent said, so an extent
      // that is *illegible* means the walk may already be out of step and this
      // address may be somebody's payload -- and rebuilding from it writes a
      // control word into that payload. Measured, letting the patrol do it cost
      // 565 trials of silent corruption in the `fast` alloc_hdr cells, against
      // 3 when it declines.
      //
      // A legible extent with a failed checksum is the opposite case: the walk
      // is in step and only this block's contents are in doubt, which is
      // precisely what recovery is for. That is the line mm_is_block draws, and
      // under `fast` -- where mm_header_ok is that same bounds check -- means
      // the patrol never rebuilds, which is the honest answer for a profile
      // with nothing to tell a damaged block from a run of payload bytes.
      //
      // Nothing is lost by declining. The first free of a neighbour reaches the
      // same damage through mm_publish, from an owner the allocator does vouch
      // for.
      if (mm_is_block(b)) (void)mm_rescue(b);
      if (!mm_header_ok(b)) {
        p = s->lo;  // nothing legible here; start this span's lap again
        continue;
      }
    }

    mm_status_t st = scrub_block(b);
    if (st != MM_OK) {
      if (found == MM_OK) found = st;
      hits++;
    }

    visited++;
    p += mm_block_size(b);
  }

  g_arena.scrub_span = s;
  g_arena.scrub_at = p;
  MM_STAT_NOTE(MM_STAT_SCRUB, visited);
  if (hits > 0) MM_STAT_NOTE(MM_STAT_SCRUB_HIT, hits);
  if (found != MM_OK) return mm_fail(found);
  return MM_OK;
}

void mm_scrub_tick(void) {
  // Advanced first and unconditionally, because it is not the patrol's: it is
  // the arena's count of public calls, and page trimming is rate-limited
  // against it whether or not the patrol is running at all.
  g_arena.ops++;
  if (g_scrub_interval == 0 || !mm_arena_live()) return;
  if (++g_ops_since_scrub < g_scrub_interval) return;
  g_ops_since_scrub = 0;
  (void)scrub_run(g_scrub_budget);
}

mm_status_t mm_scrub(size_t budget_blocks) {
  mm_clear_error();
  if (!mm_arena_live()) return mm_fail(MM_ERR_NOT_INITIALIZED);
  return scrub_run(budget_blocks);
}

// --- Pointer recovery ------------------------------------------------------

bool mm_owns(const void *ptr) {
  // Deliberately the whole of the test, and deliberately silent. Anything
  // inside one of our spans is ours to deal with, block start or not: a shim
  // that handed a pointer from its own arena to the system free() because it
  // was not quite where a block should begin would have turned a report into
  // an abort inside somebody else's allocator.
  return ptr != NULL && mm_span_of(ptr) != NULL;
}

mm_block *mm_block_of(const void *ptr) {
  if (!mm_arena_live()) {
    mm_fail(MM_ERR_NOT_INITIALIZED);
    return NULL;
  }
  // Find the span BEFORE reading anything near the pointer: the header lies
  // just below it, and a foreign pointer must never be dereferenced. The
  // lookup itself dereferences nothing the allocator did not map.
  const uint8_t *p = (const uint8_t *)ptr;
  const mm_span *s = p == NULL ? NULL : mm_span_of(p);
  if (s == NULL || p < s->lo + MM_HDR_SIZE) {
    mm_fail(MM_ERR_INVALID_PTR);
    return NULL;
  }
  if ((size_t)(p - (s->lo + MM_HDR_SIZE)) % MM_ALIGNMENT != 0) {
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

  // Everything checkable was checked and was sound. Under MM_MODE_LIBC that is
  // less than the whole block, and saying MM_OK would claim otherwise -- so the
  // shortfall is named. It is reported last, after every kind of damage that
  // *is* detectable, because "I could not look" must never outrank "I looked
  // and it was broken".
  if (g_arena.mode == MM_MODE_LIBC) return mm_fail(MM_ERR_DEGRADED);
  return MM_OK;
}

mm_status_t mm_check_heap(void) {
  mm_clear_error();
  if (!mm_arena_live()) return mm_fail(MM_ERR_NOT_INITIALIZED);

  size_t lost = 0;
  // What the tiling says each bin should hold, summed over every span: the
  // bins are shared, so they can only be checked against all of the tiling at
  // once. The bins are checked against this afterwards, never the other way
  // round.
  size_t per_bin[MM_BIN_COUNT] = {0};

  for (const mm_span *s = g_arena.spans; s != NULL; s = s->next) {
    size_t budget = mm_span_max_blocks(s);
    size_t covered = 0;
    // Reset per span, because each span's first block genuinely has nothing
    // before it -- that is what makes a span a span rather than part of one.
    bool have_prev = false;
    bool prev_free = false;

    // The implicit list is the walk: step block by block through the tiling
    // rather than following stored links. The links are cross-checked against
    // this afterwards, not the other way round -- geometry is the thing that
    // cannot quietly disagree with itself.
    for (uint8_t *p = s->lo; p < s->hi;) {
      if (budget-- == 0) return mm_fail(MM_ERR_CORRUPT_LINKS);

      mm_block *b = (mm_block *)(void *)p;
      if (!mm_is_block(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);
      if (!mm_header_ok(b)) return mm_fail(MM_ERR_CORRUPT_HEADER);

      // PREV_IN_USE has to agree with what actually precedes this block. The
      // first block of a span has nothing before it, and reports its
      // predecessor as in use so that nobody steps backwards off the front and
      // out of the mapping.
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

    // Blocks tile every span exactly -- quarantined space included, since
    // giving up on a block leaves it in the tiling rather than taking it out.
    // Anything else means space has gone missing rather than been surrendered
    // on purpose.
    if (covered != (size_t)(s->hi - s->lo)) {
      return mm_fail(MM_ERR_CORRUPT_LINKS);
    }
  }

  if (lost != g_arena.lost_bytes) return mm_fail(MM_ERR_CORRUPT_LINKS);

  // Now the bins, checked against what the tiling just said: every free block
  // in exactly one bin, in the bin its size asks for, no allocated block in
  // any of them, and a bitmap that agrees about which are empty.
  return mm_bins_check(per_bin);
}
