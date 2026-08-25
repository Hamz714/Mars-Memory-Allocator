// Providers, chunk growth, and the span registry. See mm_arena.h for why
// chunks are 2 MB-aligned and why the unit number is looked up rather than
// dereferenced.

// POSIX for sysconf; _DEFAULT_SOURCE for MAP_ANONYMOUS and madvise, which
// glibc hides from a strict-c11 translation unit without it. Both must come
// before any include.
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "mm_arena.h"

#include <string.h>

#if !defined(_WIN32)
#  include <sys/mman.h>
#  include <unistd.h>
#endif

// --- The platform provider -------------------------------------------------
//
// Three of them were planned: a caller-supplied buffer, mmap, and VirtualAlloc.
// The first two are here. The third is not, and saying so plainly is better
// than shipping a stub that pretends: Windows is a secondary platform for this
// project, it has no LD_PRELOAD and therefore no shim, and a growable arena
// exists to serve the shim. mm_arena_can_grow() reports that honestly and
// every caller checks it.

#if defined(_WIN32)

static void *sys_map(size_t bytes) {
  (void)bytes;
  return NULL;
}

static void sys_unmap(void *p, size_t bytes) {
  (void)p;
  (void)bytes;
}

static size_t sys_page_size(void) { return 4096; }

bool mm_arena_can_grow(void) { return false; }

void mm_arena_release_pages(uint8_t *from, uint8_t *to) {
  (void)from;
  (void)to;
}

#else

static void *sys_map(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static void sys_unmap(void *p, size_t bytes) {
  if (p != NULL && bytes != 0) (void)munmap(p, bytes);
}

static size_t sys_page_size(void) {
  long n = sysconf(_SC_PAGESIZE);
  return n > 0 ? (size_t)n : 4096;
}

bool mm_arena_can_grow(void) { return true; }

void mm_arena_release_pages(uint8_t *from, uint8_t *to) {
  if (from == NULL || to <= from) return;
  size_t page = sys_page_size();
  uintptr_t lo = mm_round_up((uintptr_t)from, page);
  uintptr_t hi = (uintptr_t)to & ~(uintptr_t)(page - 1);
  if (hi <= lo) return;
  // Failure is not an error worth reporting: the memory stays resident and
  // everything remains correct. This is a hint about residency, not a step in
  // any invariant.
  (void)madvise((void *)lo, (size_t)(hi - lo), MADV_DONTNEED);
}

#endif  // _WIN32

// --- Memory for the allocator's own structures ------------------------------
//
// Mapped once and never handed back. Everything drawn from here -- span
// descriptors, index tables, arenas -- is something a lookup running on another
// thread may still be holding a pointer to, and there is no moment at which
// that can be known to be over. Address space is not the scarce resource here;
// a use-after-free inside free() is.

void *mm_sys_alloc(size_t bytes) {
  size_t page = sys_page_size();
  return sys_map(mm_round_up(bytes, page));
}

// --- The unit index --------------------------------------------------------
//
// Open-addressed, linear-probed, keyed on the 2 MB unit a pointer falls in.
// Every unit a mapping covers gets an entry, so a pointer anywhere inside a
// span -- not only at a block start -- resolves in one probe. A miss is a
// foreign pointer, and costs the probe and nothing else.
//
// --- Why readers take no lock ----------------------------------------------
//
// Every free asks this table which span, and therefore which arena, a pointer
// belongs to. A reader-writer lock here would put an atomic read-modify-write
// on one shared cache line into every free in the program, which is exactly the
// serialisation per-thread arenas exist to remove -- the table would become the
// global lock under a different name.
//
// So readers are lock-free and writers serialise on a mutex of their own.
// Three things make that safe:
//
//   * **A table is never freed.** Growing publishes a new one and leaves the
//     old mapped, so a reader that loaded the old pointer is reading memory
//     that is still there. What it can miss is a span registered after it
//     started, which is why a miss re-reads the table pointer and tries again.
//
//   * **Deletion leaves a tombstone** rather than shifting entries back.
//     Backward-shift deletion moves entries a concurrent probe may already have
//     walked past, and there is no ordering that makes that safe. Tombstones do
//     not accumulate, because the load factor counts them and a rehash drops
//     them -- a table full of them is rebuilt at the same size rather than
//     doubled.
//
//   * **A descriptor outlives its mapping.** Descriptors come from a pool that
//     is never given back, so a reader that arrives a moment after a span was
//     released reads a descriptor that is still there and finds it disowned:
//     mm_arena_release_span clears the magic and the bounds before it unmaps
//     anything.
//
// What is left is a genuine window, and it is worth naming rather than
// implying: a lookup for an address *inside a mapping this allocator has just
// released* can still be holding that span when the memory goes. Reaching it
// requires a pointer into memory that has already been freed, which is a
// use-after-free in the caller -- the same program error that makes glibc abort
// -- and closing it would mean deferring every unmap until every thread had
// been observed to leave the allocator.

typedef struct mm_unit_slot {
  _Atomic uint64_t unit;
  // NULL is an empty slot; MM_UNIT_TOMB is one a probe walks over.
  _Atomic(mm_span *) span;
} mm_unit_slot;

#define MM_UNIT_TOMB ((mm_span *)(uintptr_t)1)

typedef struct mm_index {
  mm_unit_slot *slot;
  size_t cap;  // a power of two
} mm_index;

// Sized so that an arena of 512 chunks -- a gigabyte -- never has to grow the
// table at all. Static, because the table has to exist before the allocator can
// allocate anything.
#define MM_INDEX_STATIC 1024

static mm_unit_slot g_slots0[MM_INDEX_STATIC];
static mm_index g_index0 = {g_slots0, MM_INDEX_STATIC};
static _Atomic(mm_index *) g_index = &g_index0;

// Writers only. Readers never take it.
static mm_mutex g_index_lock = MM_MUTEX_INITIALIZER;
static size_t g_index_used;  // live entries plus tombstones
static size_t g_index_live;

static uint64_t unit_of(const void *p) {
  return (uint64_t)(uintptr_t)p >> MM_CHUNK_SHIFT;
}

// splitmix64's finalizer. The unit numbers of consecutive mappings differ in
// their low bits only, which is exactly the pattern linear probing handles
// worst, so they are mixed before they are masked.
static size_t slot_for(uint64_t unit, size_t cap) {
  uint64_t z = unit + 0x9E3779B97F4A7C15ULL;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  z ^= z >> 31;
  return (size_t)z & (cap - 1);
}

// Writers hold g_index_lock, so the stores here need no compare-exchange --
// only the release ordering that makes a reader which sees the span also see
// the unit it was filed under.
// A tombstone is walked over and never reused, even though reusing one is free
// and obvious. A reader probing concurrently may already have passed that slot,
// and filling it behind them would hide an entry they are still looking for.
// Appending at the end of the chain cannot: whatever a reader has passed, it
// has not passed the end. Tombstones are paid for by the rehash instead, which
// is where the load factor already counts them.
static void index_put(mm_index *ix, uint64_t unit, mm_span *s) {
  size_t i = slot_for(unit, ix->cap);
  for (;;) {
    mm_span *have = atomic_load_explicit(&ix->slot[i].span,
                                         memory_order_relaxed);
    if (have == NULL) break;
    if (have != MM_UNIT_TOMB &&
        atomic_load_explicit(&ix->slot[i].unit,
                             memory_order_relaxed) == unit) {
      atomic_store_explicit(&ix->slot[i].span, s, memory_order_release);
      return;
    }
    i = (i + 1) & (ix->cap - 1);
  }
  atomic_store_explicit(&ix->slot[i].unit, unit, memory_order_relaxed);
  atomic_store_explicit(&ix->slot[i].span, s, memory_order_release);
}

// Rebuilds the table, dropping tombstones. `cap` is chosen from the live count
// rather than doubled unconditionally, so a program that maps and releases
// large blocks in a loop rehashes at the same size for ever instead of growing
// without bound. Returns false when the platform will not supply the memory,
// which is the one way registration fails.
static bool index_rehash(void) {
  size_t cap = MM_INDEX_STATIC;
  while (cap < (g_index_live + 1) * 4) cap *= 2;

  mm_index *ix = (mm_index *)mm_sys_alloc(sizeof(mm_index) +
                                          cap * sizeof(mm_unit_slot));
  if (ix == NULL) return false;
  ix->slot = (mm_unit_slot *)(void *)(ix + 1);
  ix->cap = cap;
  memset(ix->slot, 0, cap * sizeof(mm_unit_slot));

  mm_index *old = atomic_load_explicit(&g_index, memory_order_relaxed);
  for (size_t i = 0; i < old->cap; i++) {
    mm_span *s = atomic_load_explicit(&old->slot[i].span,
                                      memory_order_relaxed);
    if (s != NULL && s != MM_UNIT_TOMB) {
      index_put(ix, atomic_load_explicit(&old->slot[i].unit,
                                         memory_order_relaxed), s);
    }
  }

  // The old table stays mapped. A reader holding it sees every span registered
  // before this moment, and re-reads this pointer when it does not find what it
  // was looking for.
  atomic_store_explicit(&g_index, ix, memory_order_release);
  g_index_used = g_index_live;
  return true;
}

static bool index_add(uint64_t unit, mm_span *s) {
  // Kept below half full, tombstones included. Linear probing degrades sharply
  // past that, and a probe that has degraded is a free() that has got slower.
  if ((g_index_used + 1) * 2 >= atomic_load_explicit(
          &g_index, memory_order_relaxed)->cap && !index_rehash()) {
    return false;
  }
  index_put(atomic_load_explicit(&g_index, memory_order_relaxed), unit, s);
  g_index_used++;
  g_index_live++;
  return true;
}

static void index_del(uint64_t unit) {
  mm_index *ix = atomic_load_explicit(&g_index, memory_order_relaxed);
  size_t i = slot_for(unit, ix->cap);
  for (size_t probes = 0; probes < ix->cap; probes++) {
    mm_span *s = atomic_load_explicit(&ix->slot[i].span, memory_order_relaxed);
    if (s == NULL) return;
    if (s != MM_UNIT_TOMB &&
        atomic_load_explicit(&ix->slot[i].unit, memory_order_relaxed) == unit) {
      atomic_store_explicit(&ix->slot[i].span, MM_UNIT_TOMB,
                            memory_order_release);
      g_index_live--;
      return;
    }
    i = (i + 1) & (ix->cap - 1);
  }
}

// The lock-free half. Returns the span filed under `unit`, or NULL.
static mm_span *index_find(uint64_t unit) {
  for (;;) {
    mm_index *ix = atomic_load_explicit(&g_index, memory_order_acquire);
    size_t i = slot_for(unit, ix->cap);
    mm_span *found = NULL;
    for (size_t probes = 0; probes < ix->cap; probes++) {
      mm_span *s = atomic_load_explicit(&ix->slot[i].span,
                                        memory_order_acquire);
      if (s == NULL) break;  // a miss: nothing was ever filed past here
      if (s != MM_UNIT_TOMB &&
          atomic_load_explicit(&ix->slot[i].unit,
                               memory_order_relaxed) == unit) {
        found = s;
        break;
      }
      i = (i + 1) & (ix->cap - 1);
    }
    if (found != NULL) return found;
    // A miss on a table that has since been replaced is not an answer: the
    // entry may only ever have been written to the new one.
    if (atomic_load_explicit(&g_index, memory_order_acquire) == ix) return NULL;
  }
}

// --- Span descriptors -------------------------------------------------------
//
// Out of the mapping they describe, which is a change threads forced and an
// improvement anyway. A descriptor inside the arena is memory an overrun can
// reach; a descriptor outside it is not. What threads need is the other half:
// the descriptor has to stay readable after its mapping has gone, so that a
// lookup which arrives a moment too late reads a disowned descriptor rather
// than an unmapped page.
//
// Recycled, so the pool is bounded by the number of spans in existence at once
// rather than by the number ever created. A recycled descriptor answers
// correctly for whatever it describes now, and a stale reader's bounds check
// fails against it -- which is the answer it should have got.

static mm_span *g_span_pool;   // descriptors nobody is using
static uint8_t *g_span_slab;   // what is left of the current page
static size_t g_span_slab_left;

// The caller holds g_index_lock.
static mm_span *span_desc_alloc(void) {
  if (g_span_pool != NULL) {
    mm_span *s = g_span_pool;
    g_span_pool = s->next;
    memset(s, 0, sizeof(*s));
    return s;
  }
  if (g_span_slab_left < sizeof(mm_span)) {
    size_t page = sys_page_size();
    g_span_slab = (uint8_t *)mm_sys_alloc(page);
    if (g_span_slab == NULL) return NULL;
    g_span_slab_left = page;
  }
  mm_span *s = (mm_span *)(void *)g_span_slab;
  g_span_slab += sizeof(mm_span);
  g_span_slab_left -= sizeof(mm_span);
  memset(s, 0, sizeof(*s));
  return s;
}

static void span_desc_free(mm_span *s) {
  mm_mutex_lock(&g_index_lock);
  s->next = g_span_pool;
  g_span_pool = s;
  mm_mutex_unlock(&g_index_lock);
}

// --- The registry ----------------------------------------------------------

// The caller-supplied span, if one is installed. Kept out of the unit index
// deliberately: it is not chunk-aligned, there is at most one of it, and a
// 4 GB buffer would otherwise cost two thousand table entries to describe
// something a single range check answers.
static _Atomic(mm_span *) g_user;

bool mm_span_register(mm_span *s) {
  if (s->kind == MM_SPAN_USER) {
    atomic_store_explicit(&g_user, s, memory_order_release);
    return true;
  }
  uint64_t first = unit_of(s->map_base);
  uint64_t last = unit_of((const uint8_t *)s->map_base + s->map_size - 1);
  mm_mutex_lock(&g_index_lock);
  bool ok = true;
  for (uint64_t u = first; u <= last && ok; u++) {
    if (!index_add(u, s)) {
      for (uint64_t v = first; v < u; v++) index_del(v);
      ok = false;
    }
  }
  mm_mutex_unlock(&g_index_lock);
  return ok;
}

void mm_span_unregister(mm_span *s) {
  if (g_arena.span_cache == s) g_arena.span_cache = NULL;
  if (s->kind == MM_SPAN_USER) {
    mm_span *want = s;
    (void)atomic_compare_exchange_strong_explicit(
        &g_user, &want, NULL, memory_order_release, memory_order_relaxed);
    return;
  }
  uint64_t first = unit_of(s->map_base);
  uint64_t last = unit_of((const uint8_t *)s->map_base + s->map_size - 1);
  mm_mutex_lock(&g_index_lock);
  for (uint64_t u = first; u <= last; u++) index_del(u);
  mm_mutex_unlock(&g_index_lock);
}

mm_span *mm_span_lookup_uncached(const void *p) {
  const uint8_t *q = (const uint8_t *)p;

  mm_span *u = atomic_load_explicit(&g_user, memory_order_acquire);
  if (u != NULL && q >= u->lo && q < u->hi) return u;

  mm_span *s = index_find(unit_of(p));
  if (s == NULL) return NULL;
  // A descriptor is drawn from a pool that is never given back, so this read is
  // always of memory that exists -- and the magic is what makes it mean
  // something. It is cleared before a span's mapping is released and it carries
  // the descriptor's own address, so a descriptor that has been disowned,
  // recycled or copied does not answer for anything.
  if (s->magic != mm_span_magic_for(s)) return NULL;
  // In the mapping, outside the tiling. Also what a recycled descriptor fails.
  if (q < s->lo || q >= s->hi) return NULL;
  return s;
}

mm_span *mm_span_lookup(const void *p) {
  mm_span *s = mm_span_lookup_uncached(p);
  // Only ever this arena's own spans. The cache is read and written under this
  // arena's lock and no other, so holding another arena's descriptor in it
  // would be holding a pointer that arena is entitled to recycle.
  if (s != NULL && s->owner == &g_arena) g_arena.span_cache = s;
  return s;
}

// --- Building a span -------------------------------------------------------

// Links a prepared descriptor into the arena and files the whole of its space
// as one free block. The caller has already reset the arena and drawn its
// secret; nothing here may run before that, since filing a block seals it.
// Returns false, having unmapped nothing, if the registry would not take it.
static bool span_adopt(mm_span *s) {
  // Set before the span is published, because the moment it is registered
  // another thread freeing a pointer into it will ask whose it is.
  s->owner = &g_arena;
  if (!mm_span_register(s)) return false;

  s->next = g_arena.spans;
  g_arena.spans = s;
  g_arena.span_count++;
  size_t usable = (size_t)(s->hi - s->lo);
  g_arena.total_bytes += usable;
  g_arena.next_index_base += usable / MM_ALIGNMENT;

  mm_block *first = (mm_block *)(void *)s->lo;
  first->word = 0;
#if MM_HAS_CRC
  first->payload_crc = 0;
#endif
  mm_set_word(first, usable, 0, false, false);
  // Nothing precedes the first block of a span, and reporting its predecessor
  // as in use is what stops anything stepping backwards off the front -- and,
  // just as importantly, out of this span and into another mapping.
  mm_set_prev_in_use(first, true);
  // `usable` was measured off the span itself, so this cannot be refused.
  (void)mm_publish(first);
  mm_bin_insert(first);

  // The patrol has to start somewhere, and a span nothing has ever pointed it
  // at would otherwise never be walked.
  if (g_arena.scrub_span == NULL) {
    g_arena.scrub_span = s;
    g_arena.scrub_at = s->lo;
  }
  return true;
}

mm_span *mm_arena_install_user(void *heap, size_t heap_size) {
  // One static descriptor, because a caller-supplied buffer must not have
  // bytes taken out of its front: mm_min_arena() and every utilisation figure
  // in bench/results/ are measured against the whole buffer.
  static mm_span user;

  uint8_t *base = (uint8_t *)heap;
  size_t usable = heap_size - MM_BLOCK_OFFSET;
  usable -= usable % MM_ALIGNMENT;

  user.owner = &g_arena;
  user.magic = mm_span_magic_for(&user);
  user.lo = base + MM_BLOCK_OFFSET;
  user.hi = user.lo + usable;
  user.next = NULL;
  user.index_base = g_arena.next_index_base;
  user.map_base = NULL;
  user.map_size = 0;
  user.kind = MM_SPAN_USER;
  // mm_init clears the buffer itself and then files a block into it, so
  // nothing here is untouched by the time anyone could ask.
  user.fresh = false;
  user.trim_after = 0;

  if (!span_adopt(&user)) return NULL;
  return &user;
}

// Maps `bytes` (a multiple of MM_CHUNK_SIZE) on a MM_CHUNK_SIZE boundary, by
// over-mapping one extra chunk and trimming both ends. Alignment is what makes
// the unit number of any interior pointer identify the mapping, so it is not
// negotiable -- see mm_arena.h.
static void *map_chunk_aligned(size_t bytes) {
  size_t over = bytes + MM_CHUNK_SIZE;
  if (over < bytes) return NULL;  // overflow

  uint8_t *raw = (uint8_t *)sys_map(over);
  if (raw == NULL) return NULL;

  uintptr_t aligned = mm_round_up((uintptr_t)raw, MM_CHUNK_SIZE);
  size_t front = (size_t)(aligned - (uintptr_t)raw);
  if (front != 0) sys_unmap(raw, front);
  size_t back = over - front - bytes;
  if (back != 0) sys_unmap((uint8_t *)aligned + bytes, back);
  return (void *)aligned;
}

// Maps a region able to hold a block of `block_size` and adopts it. `bytes`
// covers the block and the rounding up to whole chunks; the descriptor is not
// in there.
static mm_span *map_span(size_t block_size, mm_span_kind_t kind) {
  if (!mm_arena_can_grow() || !g_arena.growable) return NULL;

  size_t want = MM_BLOCK_OFFSET + block_size;
  if (want < block_size) return NULL;  // overflow
  size_t bytes = mm_round_up(want, MM_CHUNK_SIZE);
  if (bytes < want) return NULL;

  mm_mutex_lock(&g_index_lock);
  mm_span *s = span_desc_alloc();
  mm_mutex_unlock(&g_index_lock);
  if (s == NULL) return NULL;

  uint8_t *base = (uint8_t *)map_chunk_aligned(bytes);
  if (base == NULL) {
    span_desc_free(s);
    return NULL;
  }

  uint8_t *lo = base + MM_BLOCK_OFFSET;
  size_t usable = (size_t)(base + bytes - lo);
  usable -= usable % MM_ALIGNMENT;

  s->magic = mm_span_magic_for(s);
  s->lo = lo;
  s->hi = lo + usable;
  s->next = NULL;
  s->index_base = g_arena.next_index_base;
  s->map_base = base;
  s->map_size = bytes;
  // How the span was asked for, not how big it turned out. A request just over
  // half a chunk still rounds to a single chunk's worth of mapping, and it is
  // still a mapping made for one allocation and handed straight back when that
  // allocation is released -- which is what MM_SPAN_LARGE means. An ordinary
  // chunk is kept and reused instead, because churning mappings for ordinary
  // allocations trades a cheap free-list operation for two syscalls and a
  // page-fault storm.
  s->kind = (uint8_t)kind;
  s->fresh = true;
  s->trim_after = 0;

  if (!span_adopt(s)) {
    sys_unmap(base, bytes);
    span_desc_free(s);
    return NULL;
  }
  g_arena.fresh_spans++;
  return s;
}

int mm_arena_init_growable(void) {
  if (!mm_arena_can_grow()) return -1;

  // Every arena, not only this thread's: this is the same lifecycle call
  // mm_init is, and it discards whatever was installed before.
  mm_arenas_reset();
  g_arena.secret = mm_draw_secret();
  g_arena.mode = MM_MODE_MANAGED;
  g_arena.growable = true;
  if (map_span(MM_MIN_BLOCK, MM_SPAN_CHUNK) == NULL) {
    g_arena.growable = false;
    return -1;
  }
  return 0;
}

mm_span *mm_arena_grow(size_t block_size) {
  if (block_size > MM_LARGE_THRESHOLD) return NULL;
  return map_span(block_size, MM_SPAN_CHUNK);
}

mm_span *mm_arena_map_large(size_t block_size) {
  return map_span(block_size, MM_SPAN_LARGE);
}

// --- Release ---------------------------------------------------------------

size_t mm_span_take_fresh(const mm_block *b) {
  mm_span *s = mm_span_of(b);
  if (s == NULL || !s->fresh) return SIZE_MAX;

  s->fresh = false;
  g_arena.fresh_spans--;
  // The span was fresh, so the only free block in it covered the whole of it
  // and started at s->lo. A block starting anywhere else means that reasoning
  // does not hold, and the honest answer is then to promise nothing.
  if ((const uint8_t *)(const void *)b != s->lo) return SIZE_MAX;
  return MM_FRESH_DIRTY_PREFIX;
}

void mm_arena_release_span(mm_span *s) {
  if (s == NULL || s->kind == MM_SPAN_USER) return;

  if (s->fresh) g_arena.fresh_spans--;
  mm_scrub_leave(s);
  mm_span_unregister(s);

  for (mm_span **link = &g_arena.spans; *link != NULL; link = &(*link)->next) {
    if (*link == s) {
      *link = s->next;
      break;
    }
  }
  g_arena.span_count--;
  g_arena.total_bytes -= (size_t)(s->hi - s->lo);

  void *base = s->map_base;
  size_t bytes = s->map_size;
  // Disowned *before* the memory goes, and in this order. A lookup on another
  // thread may already be holding this descriptor; what it must not do is
  // conclude that an address is inside a span whose pages are about to be
  // unmapped. Clearing the bounds first makes its range check fail, and
  // clearing the magic makes the whole descriptor stop answering.
  s->lo = NULL;
  s->hi = NULL;
  s->magic = 0;
  sys_unmap(base, bytes);
  // Only now, so that a descriptor cannot be recycled into a live span while
  // another thread is still reading it as this one.
  span_desc_free(s);
}

void mm_arena_reset(void) {
  mm_span *s = g_arena.spans;
  while (s != NULL) {
    mm_span *next = s->next;
    if (s->kind == MM_SPAN_USER) {
      mm_span_unregister(s);
    } else {
      mm_span_unregister(s);
      void *base = s->map_base;
      size_t bytes = s->map_size;
      s->lo = NULL;
      s->hi = NULL;
      s->magic = 0;
      sys_unmap(base, bytes);
      span_desc_free(s);
    }
    s = next;
  }

  g_arena.spans = NULL;
  g_arena.span_count = 0;
  g_arena.fresh_spans = 0;
  g_arena.total_bytes = 0;
  g_arena.span_cache = NULL;
  g_arena.scrub_span = NULL;
  g_arena.scrub_at = NULL;
  g_arena.growable = false;
  g_arena.lost_bytes = 0;
  // Back to zero, so a caller-supplied arena's block indices are its offsets
  // and nothing else. The fault injector replays a run from a seed and a
  // pinned secret, and an index that depended on how many arenas this process
  // had installed before would make that replay depend on it too.
  g_arena.next_index_base = 0;
  mm_bins_reset();
}
