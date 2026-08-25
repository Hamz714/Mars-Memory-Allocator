// Where the memory comes from: providers, chunk growth, and the span registry.
//
// --- Why chunks are 2 MB and 2 MB-aligned ----------------------------------
//
// Every operation that starts from a pointer -- free, realloc, verify, and in
// the shim the question "is this even ours?" -- has to find the span that
// pointer belongs to before it may touch anything. Doing that by walking a list
// of regions makes the cost of a free proportional to how much memory the
// program has, which is the opposite of what an allocator is for.
//
// The alternative allocators reach for is a bit in the header saying which
// region a block came from. This one does not have a bit to spare, and would
// not want to spend one: the control word is checksummed and every field in it
// is already load-bearing.
//
// Chunk alignment gets the same answer for free. Every chunk is a whole number
// of 2 MB units starting on a 2 MB boundary, so
//
//     unit = (uintptr_t)p >> MM_CHUNK_SHIFT
//
// identifies the mapping a pointer is in with a shift, spending zero header
// bits. That is the property brief 05's per-thread arenas need, and it is far
// cheaper to build in now than to retrofit -- retrofitting it means changing
// the header again.
//
// --- What the unit number is looked up in, and why it is not a dereference --
//
// The obvious next step, `chunk = (mm_chunk *)((uintptr_t)p & ~(CHUNK - 1))`,
// is a dereference of memory chosen by the caller. That is safe for a pointer
// this allocator handed out and unsafe for every other kind, and "every other
// kind" is precisely what a libc shim is handed: a program may free something
// allocated before the shim was loaded, and the 2 MB-aligned address below that
// pointer need not be mapped at all. Reading it to find out whether we own it
// segfaults inside free, which is the worst possible place to segfault.
//
// So the unit number indexes an open-addressed table of spans this allocator
// mapped itself. A miss is a foreign pointer and costs one probe; a hit yields
// a span descriptor we own, and only then is anything dereferenced. The shift
// is still what makes the key, so the alignment is still doing the work the
// design asked of it -- the table is what makes asking safe.
//
// --- And under threads it is what makes asking cheap ------------------------
//
// Every free asks the table which span, and therefore which arena, a pointer
// belongs to, so the table sits on the path of every free in the program. That
// is why its readers take no lock at all: a reader-writer lock here would put
// an atomic read-modify-write on one shared cache line into every free, and the
// table would have become the global lock under a different name. mm_arena.c
// says what makes lock-free reads safe.

#ifndef MARS_MM_ARENA_H_
#define MARS_MM_ARENA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mm_internal.h"

// 2 MB, which is also a transparent-hugepage boundary on x86-64.
#define MM_CHUNK_SHIFT 21
#define MM_CHUNK_SIZE ((size_t)1 << MM_CHUNK_SHIFT)

// An allocation needing more than half a chunk gets a mapping of its own
// rather than a chunk it would mostly fill. Half is the point where a chunk
// stops being shareable: below it two allocations can live in one chunk, above
// it they never can, so the chunk would be a dedicated mapping wearing a
// chunk's clothes and would never be released until the whole arena was.
#define MM_LARGE_THRESHOLD (MM_CHUNK_SIZE / 2)

// Memory for the allocator's own structures -- span descriptors, index tables,
// arenas. Mapped once and never handed back, because everything drawn from here
// is something a lookup running on another thread may still be holding a
// pointer to, and there is no moment at which that can be known to be over.
// NULL where the platform cannot map anything.
void *mm_sys_alloc(size_t bytes);

// Whether this platform can map memory of its own. False on Windows, where the
// arena is caller-supplied and fixed -- the shim is Unix-only by design, and a
// growable arena exists to serve it.
bool mm_arena_can_grow(void);

// --- The registry ----------------------------------------------------------

// Adds a span to the registry, so that mm_span_of can find it. Fails only if
// the index table cannot be grown, in which case the span is not usable and
// the caller must release it.
bool mm_span_register(mm_span *s);

// Removes it again. Safe on a span that was never registered.
void mm_span_unregister(mm_span *s);

// --- Installing an arena ---------------------------------------------------

// Installs a caller-supplied buffer as the arena's only span, releasing
// whatever was there before. The descriptor is held outside the buffer, so the
// caller's whole buffer is arena and mm_min_arena() is unchanged. Returns NULL
// only if the registry refuses it.
mm_span *mm_arena_install_user(void *heap, size_t heap_size);

// --- Growth ----------------------------------------------------------------

// Starts an arena that maps its own memory, discarding whatever was installed
// before. Returns 0 on success and -1 if the first chunk could not be mapped.
//
// This is the entry point the shim uses. mm_init is the other one, and the two
// are alternatives: a caller-supplied buffer cannot grow, and an arena that
// grows has no caller-supplied buffer to start from.
int mm_arena_init_growable(void);

// Maps another chunk large enough to hold a block of `block_size` bytes, files
// its space as one free block, and returns the span. NULL when the arena is
// not growable, when `block_size` needs a dedicated mapping instead, or when
// the mapping failed.
mm_span *mm_arena_grow(size_t block_size);

// Maps a region dedicated to a single block of `block_size` bytes and returns
// the span holding it, with that block already marked in use. NULL on failure.
mm_span *mm_arena_map_large(size_t block_size);

// Hands a span's mapping back to the operating system. The span must be one
// this allocator mapped -- a caller-supplied arena is never released here --
// and must hold nothing anyone still owns.
void mm_arena_release_span(mm_span *s);

// Releases every mapped span and empties the registry. mm_init calls this
// before installing a caller-supplied arena, so switching between the two
// leaks nothing.
void mm_arena_reset(void);

// --- Pages -----------------------------------------------------------------

// Consumes the freshness of the span `b` was carved from, and reports how many
// bytes at the front of `b`'s payload the allocator wrote into it.
//
// A span is fresh while nothing has been allocated out of it, which for an
// anonymous mapping means every byte of it is still the zero the kernel
// supplied. The one exception is the block at the very start: it was the free
// block the whole span began as, so its first two payload words hold the
// free-list links that filed it. That is the prefix this returns.
//
// SIZE_MAX means nothing may be assumed. Calling this consumes the freshness
// whatever the answer, because after this block is handed out the allocator can
// no longer say which pages the caller has touched.
size_t mm_span_take_fresh(const mm_block *b);

// Hands the whole pages inside [from, to) back to the operating system without
// unmapping them, so a large free returns resident memory rather than only
// making it reusable. The pages read back as zero afterwards, which is what
// they held anyway: this is only ever called on the interior of a free block,
// never over a header, a boundary tag or a free-list link.
//
// A no-op where the platform has nothing equivalent.
void mm_arena_release_pages(uint8_t *from, uint8_t *to);

// Smallest run of free space worth handing back to the operating system. Half
// a chunk, so that a chunk which has become entirely free returns its pages
// while an ordinary allocation never comes close.
#define MM_DONTNEED_MIN (MM_CHUNK_SIZE / 2)

// And how often one span may do it: at most once per this many allocator
// calls.
//
// The size threshold alone is not enough, and the measurement is what showed
// it. A program that allocates and frees a small buffer in a loop leaves the
// whole chunk free after every single free, so every single free met the size
// test -- and each MADV_DONTNEED threw away page mappings that the very next
// allocation faulted straight back in. `calloc_4kb_x200000` in
// bench/results/preload-*.csv cost 4.2 us per call that way, against glibc's
// 0.07 us, and none of it was allocation.
//
// A watermark fixes it because the two cases differ in *rate*, not in shape: a
// program genuinely finished with a chunk frees it once, and one that is
// churning frees it constantly. Rate-limiting cannot tell them apart, and does
// not need to -- it makes the second case pay a bounded price. A program that
// has finished still gives its pages back on the next call that qualifies;
// one that is churning gives them back a thousand times less often.
//
// glibc solves the same problem with a threshold that adapts to what the
// program has been doing. This is the fixed-cost version, and it is fixed
// because an adaptive one is a measurement exercise of its own.
#define MM_TRIM_INTERVAL ((uint64_t)1024)

#endif  // MARS_MM_ARENA_H_
