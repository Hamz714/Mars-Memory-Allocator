// Locking, and the arena a thread allocates from.
//
// --- Two strategies, because one of them is the measurement ----------------
//
// `MARS_LOCK` selects one at compile time, and both are built and tested. The
// second is not here to be shipped: it is the control the design that follows
// it has to be argued against, and a design justified against a measured
// alternative is worth more than one asserted.
//
//   MM_LOCK_NONE    No locking at all. What the allocator was before this
//                   phase, kept so that a single-threaded benchmark is not
//                   paying for a mutex it never contends -- which is what makes
//                   the cost of the lock itself a number rather than a guess.
//
//   MM_LOCK_GLOBAL  One mutex around every public entry point. Correct under
//                   threads, and flat under them: every allocating thread
//                   serialises on the same lock however many cores there are.
//                   The default, because a correct allocator that does not
//                   scale is worth more than a fast one that corrupts memory.
//
// --- Why the lock is per arena rather than one global mutex ----------------
//
// There is one arena today, so the two read the same. The mutex still lives in
// the arena struct rather than beside it, because what it protects is an
// arena's block metadata and nothing else -- the bins, the tiling, the span
// list and the counters of *that* arena. Naming it there is what makes the
// next question ("could two arenas run at once?") a question about arenas
// instead of a question about a global.
//
// --- What the lock has to cover, which is more than it looks ---------------
//
// This allocator's metadata is *in band*. Freeing a block writes PREV_IN_USE
// into the header of the block after it, and allocating one does the same, so
// an operation on one block writes into a neighbour that some other thread may
// be holding a pointer to. Every one of those headers is checksummed, so the
// write is a control word and a checksum that have to move together. A reader
// racing with it does not see a torn size; it sees a word and a checksum that
// disagree -- and this allocator's answer to that is MM_ERR_CORRUPT_HEADER.
//
// So a race here does not make the allocator slightly wrong. It makes it report
// corruption that never happened, which is the one failure this project cannot
// have. Every public entry point that reads or writes block metadata is inside
// the lock, including the read-only ones: mm_verify and mm_check_heap are
// exactly the calls a racy header would lie to.

#ifndef MARS_MM_LOCK_H_
#define MARS_MM_LOCK_H_

#include <stdbool.h>
#include <stddef.h>

#define MM_LOCK_NONE 0
#define MM_LOCK_GLOBAL 1

// A header that assumes it was configured is a header that silently compiles a
// single-threaded allocator into a threaded program. The default is the safe
// one for the same reason CMake's is.
#ifndef MM_LOCK
#  define MM_LOCK MM_LOCK_GLOBAL
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

// The arena is a static object, so its lock is initialised by the linker
// rather than by a constructor nobody could be sure had run: the preload shim
// is entered by the dynamic loader before any constructor of its own.
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

// "none" or "global". Recorded in the benchmark CSVs, because two runs are only
// comparable when it matches.
const char *mm_lock_strategy(void);

// How many arenas exist. One, today, under either strategy -- exposed so that
// the tests state that as a fact they check rather than an assumption they
// carry.
size_t mm_arena_count(void);

#endif  // MARS_MM_LOCK_H_
