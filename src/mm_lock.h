// Locking, and the arena a thread allocates from.
//
// --- Three strategies, because two of them are the measurement -------------
//
// `MARS_LOCK` selects one at compile time. All three are built and tested, and
// the middle one is not there to be shipped: it is the control the third has to
// be argued against. A design justified against a measured alternative is worth
// more than one asserted, and both curves are in bench/results/.
//
//   MM_LOCK_NONE    No locking at all. What the allocator was before this
//                   phase, kept so that a single-threaded benchmark is not
//                   paying for a mutex it never contends -- which is what makes
//                   the cost of the lock a number rather than a guess.
//
//   MM_LOCK_GLOBAL  One arena, one mutex, every public entry point inside it.
//                   Correct under threads and flat under them: every thread
//                   serialises on the same lock however many cores there are.
//
//   MM_LOCK_ARENA   One arena per thread, each with its own mutex, and a
//                   lock-free queue for frees that cross a thread boundary.
//                   The default.
//
// --- Why an arena still has a mutex under MM_LOCK_ARENA --------------------
//
// The obvious per-thread design has the owning thread take no lock at all. It
// does not work here, and the reason is this allocator's metadata rather than
// anything about threads.
//
// The metadata is *in band*. Freeing a block writes PREV_IN_USE into the header
// of the block after it, and allocating one does the same -- so a thread
// working in its own arena writes into the headers of blocks *other* threads
// are holding, whenever those blocks came from this arena. Every one of those
// headers is checksummed, so the write is a control word and a checksum that
// have to move together. A reader racing with it does not see a torn size; it
// sees a word and a checksum that disagree, and this allocator's answer to that
// is MM_ERR_CORRUPT_HEADER.
//
// So a race here does not make the allocator slightly wrong. It makes it report
// corruption that never happened, which is the one failure this project cannot
// have. The rule that follows is: **a block's metadata is only ever touched
// under its own arena's lock.**
//
// For the thread that owns the arena that lock is uncontended, and an
// uncontended pthread mutex is two atomic operations and no syscall. What
// changed between the two strategies is not whether there is a lock but who
// else is waiting on it, and the curves in docs/RESULTS.md are what that
// difference is worth.
//
// --- The one operation that must not take the owner's lock -----------------
//
// `free` from a thread that does not own the block. That is what a producer /
// consumer program does to every single object it allocates, and taking the
// producer's lock there would serialise exactly what per-thread arenas exist to
// unserialise. It goes on the owning arena's remote-free queue instead, and the
// owner drains it on its next call.
//
// See mm_internal.h for why that queue is a bounded ring of pointers rather
// than the usual list threaded through the freed blocks themselves.

#ifndef MARS_MM_LOCK_H_
#define MARS_MM_LOCK_H_

#include <stdbool.h>
#include <stddef.h>

#define MM_LOCK_NONE 0
#define MM_LOCK_GLOBAL 1
#define MM_LOCK_ARENA 2

// A header that assumes it was configured is a header that silently compiles a
// single-threaded allocator into a threaded program. The default is the safe
// one, for the same reason CMake's is.
#ifndef MM_LOCK
#  define MM_LOCK MM_LOCK_ARENA
#endif

#if MM_LOCK == MM_LOCK_NONE

// Not an empty struct: C11 has no such thing, and a zero-sized member is a GNU
// extension in a tree compiled with C_EXTENSIONS OFF.
typedef struct mm_mutex {
  char unused;
} mm_mutex;

#  define MM_MUTEX_INITIALIZER {0}

static inline void mm_mutex_init(mm_mutex *m) { (void)m; }
static inline void mm_mutex_lock(mm_mutex *m) { (void)m; }
static inline void mm_mutex_unlock(mm_mutex *m) { (void)m; }

#else

#  include <pthread.h>

typedef pthread_mutex_t mm_mutex;

// The arena the process starts on is a static object, so its lock is
// initialised by the linker rather than by a constructor nobody could be sure
// had run: a preload shim is entered by the dynamic loader before any
// constructor of its own.
#  define MM_MUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER

// The return values are dropped deliberately rather than by omission. A mutex
// that will not lock leaves nothing correct to do inside malloc -- there is
// nobody to report it to and no way to proceed -- and for a process-private
// mutex the only ways these fail are already undefined behaviour.
static inline void mm_mutex_init(mm_mutex *m) {
  (void)pthread_mutex_init(m, NULL);
}
static inline void mm_mutex_lock(mm_mutex *m) { (void)pthread_mutex_lock(m); }
static inline void mm_mutex_unlock(mm_mutex *m) {
  (void)pthread_mutex_unlock(m);
}

#endif  // MM_LOCK == MM_LOCK_NONE

// "none", "global" or "arena". Recorded in the benchmark CSVs, because two runs
// are only comparable when it matches.
const char *mm_lock_strategy(void);

// How many arenas exist. One under the first two strategies. Under
// MM_LOCK_ARENA it is one per thread that has allocated -- unless the arena the
// process started on cannot grow, which is the case for a caller-supplied
// buffer, and then it stays one and every thread shares it.
size_t mm_arena_count(void);

struct mm_arena;

// The arena this thread should work in, adopting or creating one if this is the
// thread's first call. Out of line because it runs once per thread; the inline
// mm_enter in mm_internal.h is what every call after that goes through.
struct mm_arena *mm_arena_adopt(void);

// Empties every arena and unclaims it, so that the next call in each thread
// starts again from whatever mm_init has just installed. Part of mm_init, which
// is a lifecycle call: it cannot be made safe against another thread allocating
// at the same time, and nothing here pretends otherwise.
void mm_arenas_reset(void);

// The arenas, oldest first, for the whole-heap operations -- mm_check_heap,
// mm_stats_get, mm_set_mode -- which are the only things that have any business
// looking at an arena other than their own. Never NULL: the arena the process
// started on is always there.
struct mm_arena *mm_arena_first(void);
struct mm_arena *mm_arena_next(struct mm_arena *a);

#endif  // MARS_MM_LOCK_H_
