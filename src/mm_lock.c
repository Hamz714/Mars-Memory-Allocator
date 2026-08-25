// The arenas: the one the process starts on, the ones threads take for
// themselves, and the queue a free uses when it crosses between them.
//
// See mm_lock.h for what the three strategies are and why the per-thread one
// still has a mutex.

#include "mm_internal.h"

#include <string.h>

#if MM_LOCK == MM_LOCK_ARENA
#  include <pthread.h>
#endif

#include "mm_arena.h"

// Statically initialised, lock included. A preload shim is entered by the
// dynamic loader before any constructor in it has run, so anything needing
// initialisation at run time would have to be initialised from inside malloc --
// and initialising a mutex from inside the thing the mutex protects is not a
// problem with a solution.
mm_arena g_main_arena = {.lock = MM_MUTEX_INITIALIZER};

uint64_t g_arena_epoch;

const char *mm_lock_strategy(void) {
#if MM_LOCK == MM_LOCK_NONE
  return "none";
#elif MM_LOCK == MM_LOCK_GLOBAL
  return "global";
#else
  return "arena";
#endif
}

#if MM_LOCK != MM_LOCK_ARENA

// One arena, and every thread in it. mm_arena_adopt is never reached from
// mm_enter, which names g_main_arena directly.

size_t mm_arena_count(void) { return 1; }

mm_arena *mm_arena_adopt(void) { return &g_main_arena; }

mm_arena *mm_arena_first(void) { return &g_main_arena; }

mm_arena *mm_arena_next(mm_arena *a) {
  (void)a;
  return NULL;
}

void mm_arenas_reset(void) {
  g_arena_epoch++;
  mm_mutex_lock(&g_main_arena.lock);
  mm_arena_reset();
  mm_mutex_unlock(&g_main_arena.lock);
}

mm_guard mm_enter_for(const void *ptr) {
  (void)ptr;
  mm_guard g;
  g.arena = mm_enter();
  g.saved = NULL;
  g.swapped = false;
  return g;
}

void mm_leave_for(mm_guard g) { mm_leave(g.arena); }

mm_guard mm_enter_arena(mm_arena *a) {
  mm_guard g;
  mm_mutex_lock(&a->lock);
  g.arena = a;
  g.saved = NULL;
  g.swapped = false;
  return g;
}

mm_arena *mm_owner_of(const void *ptr) {
  return ptr != NULL && mm_span_lookup_uncached(ptr) != NULL ? &g_main_arena
                                                            : NULL;
}

bool mm_remote_push(mm_arena *a, void *payload) {
  (void)a;
  (void)payload;
  return false;  // there is nowhere for a free to cross to
}

void mm_remote_forget(mm_arena *a) { (void)a; }

void *mm_remote_pop(mm_arena *a) {
  (void)a;
  return NULL;
}

#else  // MM_LOCK == MM_LOCK_ARENA

_Thread_local mm_arena *mm_self __attribute__((tls_model("initial-exec")));

// Guards the arena list and the pool of arenas nobody is using. Outside both of
// the other locks: it is never held while an arena's lock is held, and never
// taken while one is. The full order is this lock, then an arena's, then the
// span registry's, and no path takes them in any other sequence.
static mm_mutex g_arenas_lock = MM_MUTEX_INITIALIZER;
static mm_arena *g_arenas = &g_main_arena;
static size_t g_arena_count = 1;

// Arenas whose thread has gone. They keep every block that was allocated out of
// them -- a thread exiting does not invalidate memory other threads are still
// holding -- and go to the next thread that needs one, which is what stops a
// program creating threads in a loop from accumulating an arena per thread.
static mm_arena *g_orphans;

size_t mm_arena_count(void) {
  mm_mutex_lock(&g_arenas_lock);
  size_t n = g_arena_count;
  mm_mutex_unlock(&g_arenas_lock);
  return n;
}

// The list is only ever appended to and never unlinked from, so walking it
// needs no lock -- which is what lets mm_check_heap take one arena's lock at a
// time rather than holding the list still while it walks every one of them.
mm_arena *mm_arena_first(void) { return g_arenas; }

mm_arena *mm_arena_next(mm_arena *a) { return a->next_arena; }

// --- Giving an arena back when its thread ends ------------------------------

static pthread_key_t g_thread_key;
static pthread_once_t g_thread_key_once = PTHREAD_ONCE_INIT;

// The arena is not destroyed and its memory is not reclaimed. That is the
// point: tests/test_threads.c has a thread allocate, exit, and the blocks it
// left behind are still readable, still verifiable and still freeable
// afterwards. All that happens here is that the arena stops being spoken for.
//
// A thread that allocates *after* its own destructor has run keeps using the
// arena it had, which another thread may by then have adopted. That is safe
// rather than merely unlikely: two threads in one arena is the caller-supplied
// case, and every operation takes the arena's lock.
static void on_thread_exit(void *value) {
  mm_arena *a = (mm_arena *)value;
  if (a == NULL || a == &g_main_arena) return;
  mm_mutex_lock(&g_arenas_lock);
  if (a->claimed) {
    a->claimed = false;
    a->next_orphan = g_orphans;
    g_orphans = a;
  }
  mm_mutex_unlock(&g_arenas_lock);
}

static void make_thread_key(void) {
  (void)pthread_key_create(&g_thread_key, on_thread_exit);
}

// --- Adoption ---------------------------------------------------------------

// Room for one arena. Mapped rather than allocated, because the thing being
// created *is* the allocator: there is nothing to allocate from yet.
static mm_arena *arena_new(void) {
  mm_arena *a = (mm_arena *)mm_sys_alloc(sizeof(mm_arena));
  if (a == NULL) return NULL;
  memset(a, 0, sizeof(*a));
  mm_mutex_init(&a->lock);
  mm_remote_forget(a);
  return a;
}

mm_arena *mm_arena_adopt(void) {
  (void)pthread_once(&g_thread_key_once, make_thread_key);

  mm_mutex_lock(&g_arenas_lock);

  mm_arena *a = NULL;
  if (!g_main_arena.claimed) {
    // Whoever gets here first works in the arena the process started on -- the
    // thread that called mm_init or loaded the shim, in any program with a main
    // thread. A single-threaded program therefore has exactly one arena and
    // exactly the behaviour it had before this phase.
    a = &g_main_arena;
    mm_remote_forget(a);
  } else if (!g_main_arena.growable) {
    // A caller-supplied buffer is one fixed region and cannot be divided, so
    // there is nothing to give this thread of its own. Every thread shares the
    // arena and contends for its lock, which is MM_LOCK_GLOBAL's behaviour
    // arrived at honestly rather than a per-thread design quietly not
    // happening. mm_arena_count() stays at 1 and says so.
    mm_mutex_unlock(&g_arenas_lock);
    mm_self = &g_main_arena;
    return &g_main_arena;
  } else if (g_orphans != NULL) {
    a = g_orphans;
    g_orphans = a->next_orphan;
    a->next_orphan = NULL;
  }

  if (a == NULL) {
    a = arena_new();
    if (a == NULL) {
      // Nothing left to map. Sharing the main arena is slower and correct,
      // which is the right way round for a failure here to go.
      mm_mutex_unlock(&g_arenas_lock);
      mm_self = &g_main_arena;
      return &g_main_arena;
    }
    mm_arena **link = &g_arenas;
    while (*link != NULL) link = &(*link)->next_arena;
    // Published last, and after everything else about the arena is set, because
    // mm_check_heap walks this list without the list lock.
    *link = a;
    g_arena_count++;
  }

  a->claimed = true;
  a->epoch = g_arena_epoch;
  mm_mutex_unlock(&g_arenas_lock);

  mm_self = a;
  (void)pthread_setspecific(g_thread_key, a);

  // An arena with no memory yet -- newly made, or recycled from a thread that
  // has gone and then emptied by mm_init -- is given some here rather than on
  // its first allocation. mm_arena_live() is what every entry point checks, and
  // a growable arena with no spans would fail that check before it ever reached
  // the code that would have grown it.
  if (a != &g_main_arena && a->spans == NULL) {
    mm_mutex_lock(&a->lock);
    a->secret = mm_draw_secret();
    a->mode = mm_mode_default();
    a->growable = true;
    if (mm_arena_grow(MM_MIN_BLOCK) == NULL) a->growable = false;
    mm_mutex_unlock(&a->lock);
  }
  return a;
}

void mm_arenas_reset(void) {
  mm_mutex_lock(&g_arenas_lock);
  g_arena_epoch++;

  for (mm_arena *a = g_arenas; a != NULL; a = a->next_arena) {
    // mm_arena_reset works on g_arena, which is this thread's idea of where it
    // is. Pointing it at each arena in turn is how one thread tears down all of
    // them without every function underneath having to take an arena argument.
    mm_self = a;
    mm_mutex_lock(&a->lock);
    mm_arena_reset();
    // The queued frees refer to blocks in arenas that have just stopped
    // existing. Freeing them would be freeing memory that has been unmapped.
    mm_remote_forget(a);
    mm_mutex_unlock(&a->lock);
    a->claimed = false;
    a->next_orphan = NULL;
  }

  // Everything but the arena the process started on goes back into the pool.
  // The epoch was bumped above, so a thread still holding one of them adopts
  // again on its next call rather than allocating out of one that was emptied
  // underneath it.
  g_orphans = NULL;
  mm_arena **link = &g_orphans;
  for (mm_arena *a = g_arenas->next_arena; a != NULL; a = a->next_arena) {
    *link = a;
    link = &a->next_orphan;
  }
  *link = NULL;

  g_main_arena.claimed = true;
  g_main_arena.epoch = g_arena_epoch;
  mm_mutex_unlock(&g_arenas_lock);

  mm_self = &g_main_arena;
}

// --- Working in somebody else's arena ---------------------------------------

mm_guard mm_enter_for(const void *ptr) {
  mm_guard g;
  mm_arena *owner = mm_owner_of(ptr);
  if (owner == NULL || owner == mm_self) {
    g.saved = NULL;
    g.swapped = false;
    g.arena = mm_enter();
    return g;
  }
  // g_arena names the owner for the duration, so everything this call touches
  // -- the bins, lost_bytes, the counters, the patrol cursor -- lands in the
  // arena the block actually belongs to rather than in ours. Nothing reachable
  // from here allocates, which is what keeps the one-lock-at-a-time rule.
  g.saved = mm_self;
  g.swapped = true;
  mm_self = owner;
  mm_mutex_lock(&owner->lock);
  g.arena = owner;
  return g;
}

void mm_leave_for(mm_guard g) {
  mm_mutex_unlock(&g.arena->lock);
  // Restored even when it was NULL. A thread whose first allocator call is a
  // question about somebody else's pointer has no arena of its own yet, and
  // must not come out of this owning one.
  if (g.swapped) mm_self = g.saved;
}

mm_guard mm_enter_arena(mm_arena *a) {
  mm_guard g;
  g.saved = mm_self;
  g.swapped = true;
  mm_self = a;
  mm_mutex_lock(&a->lock);
  g.arena = a;
  return g;
}

mm_arena *mm_owner_of(const void *ptr) {
  if (ptr == NULL) return NULL;
  // Uncached deliberately. The per-arena span cache is read and written without
  // any lock but the arena's own, so putting another arena's span in it would
  // leave this thread holding a pointer to a descriptor that arena is entitled
  // to release. This question is asked *about* other arenas, so it is asked
  // without the cache.
  const mm_span *s = mm_span_lookup_uncached(ptr);
  return s == NULL ? NULL : s->owner;
}

// --- The remote-free queue --------------------------------------------------

bool mm_remote_push(mm_arena *a, void *payload) {
  uint32_t pos = atomic_load_explicit(&a->remote_head, memory_order_relaxed);
  for (;;) {
    mm_remote_cell *cell = &a->remote[pos & (MM_REMOTE_SLOTS - 1)];
    uint32_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
    int32_t diff = (int32_t)(seq - pos);
    if (diff == 0) {
      if (atomic_compare_exchange_weak_explicit(&a->remote_head, &pos, pos + 1,
                                                memory_order_relaxed,
                                                memory_order_relaxed)) {
        cell->ptr = payload;
        // Release, so the owner that sees this sequence also sees the pointer.
        atomic_store_explicit(&cell->seq, pos + 1, memory_order_release);
        return true;
      }
      // The compare-exchange failed and reloaded `pos` for us.
    } else if (diff < 0) {
      return false;  // full: the owner has not caught up with the last lap
    } else {
      pos = atomic_load_explicit(&a->remote_head, memory_order_relaxed);
    }
  }
}

void *mm_remote_pop(mm_arena *a) {
  uint32_t pos = atomic_load_explicit(&a->remote_tail, memory_order_relaxed);
  mm_remote_cell *cell = &a->remote[pos & (MM_REMOTE_SLOTS - 1)];
  uint32_t seq = atomic_load_explicit(&cell->seq, memory_order_acquire);
  if ((int32_t)(seq - (pos + 1)) != 0) return NULL;
  void *p = cell->ptr;
  cell->ptr = NULL;
  atomic_store_explicit(&a->remote_tail, pos + 1, memory_order_relaxed);
  // Hands the cell back to the producers, a whole lap ahead of where it was.
  atomic_store_explicit(&cell->seq, pos + MM_REMOTE_SLOTS,
                        memory_order_release);
  return p;
}

void mm_remote_forget(mm_arena *a) {
  for (uint32_t i = 0; i < MM_REMOTE_SLOTS; i++) {
    atomic_store_explicit(&a->remote[i].seq, i, memory_order_relaxed);
    a->remote[i].ptr = NULL;
  }
  atomic_store_explicit(&a->remote_head, 0, memory_order_relaxed);
  atomic_store_explicit(&a->remote_tail, 0, memory_order_relaxed);
}

#endif  // MM_LOCK == MM_LOCK_ARENA
