// The arena object itself, and what the build was configured to do about
// threads. See mm_lock.h.

#include "mm_internal.h"

// Statically initialised, lock included. A preload shim is entered by the
// dynamic loader before any constructor in it has run, so anything that needed
// initialising at run time would have to be initialised from inside malloc --
// and initialising a mutex from inside the thing the mutex protects is not a
// problem with a solution.
mm_arena g_arena = {.lock = MM_MUTEX_INITIALIZER};

const char *mm_lock_strategy(void) {
#if MM_LOCK == MM_LOCK_NONE
  return "none";
#else
  return "global";
#endif
}

size_t mm_arena_count(void) { return 1; }
