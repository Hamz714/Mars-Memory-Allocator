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

// Bytes reserved at the front of a mapping for the span descriptor itself.
// Rounded to the alignment so that the tiling behind it starts where the
// geometry expects.
#define MM_SPAN_HDR (mm_round_up(sizeof(mm_span), MM_ALIGNMENT))

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

// --- The unit index --------------------------------------------------------
//
// Open-addressed, linear-probed, keyed on the 2 MB unit a pointer falls in.
// Every unit a mapping covers gets an entry, so a pointer anywhere inside a
// span -- not only at a block start -- resolves in one probe. A miss is a
// foreign pointer, and costs the probe and nothing else.

typedef struct mm_unit_slot {
  uint64_t unit;
  mm_span *span;  // NULL marks the slot empty
} mm_unit_slot;

// Sized so that an arena of 512 chunks -- a gigabyte -- never has to grow the
// table at all. Static, because the table has to exist before the allocator
// can allocate anything.
#define MM_INDEX_STATIC 1024

static mm_unit_slot g_index_static[MM_INDEX_STATIC];
static mm_unit_slot *g_index = g_index_static;
static size_t g_index_cap = MM_INDEX_STATIC;
static size_t g_index_len;
static size_t g_index_map_bytes;  // non-zero when the table itself is mapped

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

static void index_put(mm_unit_slot *tab, size_t cap, uint64_t unit,
                      mm_span *s) {
  size_t i = slot_for(unit, cap);
  while (tab[i].span != NULL) {
    if (tab[i].unit == unit) {  // re-registering the same unit
      tab[i].span = s;
      return;
    }
    i = (i + 1) & (cap - 1);
  }
  tab[i].unit = unit;
  tab[i].span = s;
}

// Doubles the table. Returns false when the platform cannot supply the memory,
// which is the one way registration fails.
static bool index_grow(void) {
  size_t cap = g_index_cap * 2;
  size_t bytes = mm_round_up(cap * sizeof(mm_unit_slot), sys_page_size());
  mm_unit_slot *tab = (mm_unit_slot *)sys_map(bytes);
  if (tab == NULL) return false;
  memset(tab, 0, bytes);

  for (size_t i = 0; i < g_index_cap; i++) {
    if (g_index[i].span != NULL) {
      index_put(tab, cap, g_index[i].unit, g_index[i].span);
    }
  }

  mm_unit_slot *old = g_index;
  size_t old_bytes = g_index_map_bytes;
  g_index = tab;
  g_index_cap = cap;
  g_index_map_bytes = bytes;
  if (old_bytes != 0) sys_unmap(old, old_bytes);
  return true;
}

static bool index_add(uint64_t unit, mm_span *s) {
  // Kept below half full. Linear probing degrades sharply past that, and a
  // probe that has degraded is a free() that has got slower.
  if ((g_index_len + 1) * 2 >= g_index_cap && !index_grow()) return false;
  index_put(g_index, g_index_cap, unit, s);
  g_index_len++;
  return true;
}

static size_t index_find(uint64_t unit) {
  size_t i = slot_for(unit, g_index_cap);
  for (size_t probes = 0; probes < g_index_cap; probes++) {
    if (g_index[i].span == NULL) return g_index_cap;  // a miss
    if (g_index[i].unit == unit) return i;
    i = (i + 1) & (g_index_cap - 1);
  }
  return g_index_cap;
}

// Backward-shift deletion. Tombstones would be simpler and would make every
// later probe pay for every span ever released, which under a program that
// allocates and frees large buffers in a loop is unbounded.
static void index_del(uint64_t unit) {
  size_t i = index_find(unit);
  if (i == g_index_cap) return;

  g_index[i].span = NULL;
  g_index_len--;

  size_t j = i;
  for (;;) {
    j = (j + 1) & (g_index_cap - 1);
    if (g_index[j].span == NULL) return;
    size_t home = slot_for(g_index[j].unit, g_index_cap);
    // Move j down to i when its home is not cyclically inside (i, j] -- that
    // is, when the hole at i does not separate j from where it wants to be.
    bool between = (i < j) ? (home > i && home <= j)
                           : (home > i || home <= j);
    if (!between) {
      g_index[i] = g_index[j];
      g_index[j].span = NULL;
      i = j;
    }
  }
}

// --- The registry ----------------------------------------------------------

// The caller-supplied span, if one is installed. Kept out of the unit index
// deliberately: it is not chunk-aligned, there is at most one of it, and a
// 4 GB buffer would otherwise cost two thousand table entries to describe
// something a single range check answers.
static mm_span *g_user;

bool mm_span_register(mm_span *s) {
  if (s->kind == MM_SPAN_USER) {
    g_user = s;
    return true;
  }
  uint64_t first = unit_of(s->map_base);
  uint64_t last = unit_of((const uint8_t *)s->map_base + s->map_size - 1);
  for (uint64_t u = first; u <= last; u++) {
    if (!index_add(u, s)) {
      for (uint64_t v = first; v < u; v++) index_del(v);
      return false;
    }
  }
  return true;
}

void mm_span_unregister(mm_span *s) {
  if (g_arena.span_cache == s) g_arena.span_cache = NULL;
  if (s->kind == MM_SPAN_USER) {
    if (g_user == s) g_user = NULL;
    return;
  }
  uint64_t first = unit_of(s->map_base);
  uint64_t last = unit_of((const uint8_t *)s->map_base + s->map_size - 1);
  for (uint64_t u = first; u <= last; u++) index_del(u);
}

mm_span *mm_span_lookup(const void *p) {
  const uint8_t *q = (const uint8_t *)p;

  mm_span *u = g_user;
  if (u != NULL && q >= u->lo && q < u->hi) {
    g_arena.span_cache = u;
    return u;
  }

  if (g_index_len == 0) return NULL;
  size_t i = index_find(unit_of(p));
  if (i == g_index_cap) return NULL;

  mm_span *s = g_index[i].span;
  // The descriptor lives at the front of the mapping it describes, so an
  // overrun elsewhere in the arena could in principle have reached it. It
  // carries its own address XOR-ed into a magic number for that reason: a
  // descriptor that has been overwritten, or copied from another chunk, does
  // not answer for anything.
  if (s->magic != mm_span_magic_for(s)) return NULL;
  if (q < s->lo || q >= s->hi) return NULL;  // in the mapping, outside the tiling

  g_arena.span_cache = s;
  return s;
}

// --- Building a span -------------------------------------------------------

// Links a prepared descriptor into the arena and files the whole of its space
// as one free block. The caller has already reset the arena and drawn its
// secret; nothing here may run before that, since filing a block seals it.
// Returns false, having unmapped nothing, if the registry would not take it.
static bool span_adopt(mm_span *s) {
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
// covers the descriptor, the block, and the rounding up to whole chunks.
static mm_span *map_span(size_t block_size, mm_span_kind_t kind) {
  if (!mm_arena_can_grow() || !g_arena.growable) return NULL;

  size_t want = MM_SPAN_HDR + MM_BLOCK_OFFSET + block_size;
  if (want < block_size) return NULL;  // overflow
  size_t bytes = mm_round_up(want, MM_CHUNK_SIZE);
  if (bytes < want) return NULL;

  uint8_t *base = (uint8_t *)map_chunk_aligned(bytes);
  if (base == NULL) return NULL;

  mm_span *s = (mm_span *)(void *)base;
  uint8_t *lo = base + MM_SPAN_HDR + MM_BLOCK_OFFSET;
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

  if (!span_adopt(s)) {
    sys_unmap(base, bytes);
    return NULL;
  }
  g_arena.fresh_spans++;
  return s;
}

int mm_arena_init_growable(void) {
  if (!mm_arena_can_grow()) return -1;

  mm_arena_reset();
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
  // The descriptor is inside the mapping, so nothing may be read out of `s`
  // after this line.
  s->magic = 0;
  sys_unmap(base, bytes);
}

void mm_arena_reset(void) {
  mm_span *s = g_arena.spans;
  while (s != NULL) {
    mm_span *next = s->next;
    if (s->kind == MM_SPAN_USER) {
      mm_span_unregister(s);
    } else {
      mm_span_unregister(s);
      sys_unmap(s->map_base, s->map_size);
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
