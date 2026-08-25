// A drop-in malloc family, to be LD_PRELOAD-ed into programs that were never
// built against this allocator.
//
// The point of it is evidence. A microbenchmark measures an allocator against
// a workload chosen by the person who wrote the allocator; running `git status`
// or a Python interpreter on top of it measures it against a workload chosen by
// somebody else, years ago, for reasons unrelated to any of this. That is a
// different kind of claim, and it is the only kind that says the thing works.
//
// --- What is different in here ---------------------------------------------
//
// The allocator underneath is put into MM_MODE_LIBC, and what that means is
// spelled out in include/mars/allocator.h and at the top of docs/DESIGN.md. In
// short: header checksums, canaries and free-list validation are exactly what
// they always were, and the payload is not checksummed at all -- because
// handing back a raw pointer is precisely what makes a payload checksum a lie.
//
// --- Threads ---------------------------------------------------------------
//
// **Safe under threads, and there is no lock in this file.** All of it is in
// the allocator: each thread allocates from an arena of its own, and a free
// that crosses a thread boundary is handed to the arena that owns the block.
// See src/mm_lock.h. Adding a mutex here as well would serialise every thread
// in the program on one lock in front of an allocator that no longer needs one.
//
// Two corners of *this file* are still single-threaded, and neither is reached
// by a threaded program doing ordinary work:
//
//   * **Start-up.** `g_ready`, `g_starting` and the bootstrap pool are plain
//     globals. Everything that touches them runs before the constructor below
//     has finished, which is before the program has had a chance to create a
//     thread. A library dlopen-ed from several threads at once could in
//     principle get in first; a program that does that has a race with the
//     dynamic loader as well.
//
//   * **MARS_SHIM_FLIP**, which counts allocations to stage a fault at the
//     n-th one. It is a diagnostic that exists to be pointed at one program at
//     a time, and under threads it flips a bit at an allocation nobody can
//     name rather than at the one asked for.
//
// A build with `MARS_LOCK=none` has no locking at all, and preloading *that*
// into a threaded program will corrupt its heap. The pthread_create interposer
// at the bottom exists only in that build, and only to say so.
//
// --- The traps -------------------------------------------------------------
//
// Three of these have sunk preload allocators before, and each is handled where
// it arises rather than defended against in general:
//
//   * **Bootstrap recursion.** dlsym(RTLD_NEXT, "free") allocates, through
//     calloc. If calloc calls dlsym, that is the end of it. So there is a
//     static pool, and everything allocated before the arena exists comes out
//     of it.
//
//   * **Foreign pointers.** A program can free memory it obtained before this
//     library was loaded. mm_owns answers that in O(1) and without
//     dereferencing the pointer, and anything not ours goes to the real free.
//
//   * **Reentering ourselves.** Nothing in here may call anything that
//     allocates. In particular no printf: it allocates, and a diagnostic that
//     allocates from inside malloc is a stack overflow. Every message below
//     goes out through write(2).

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE 1

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mars/allocator.h"
#include "mm_arena.h"
#include "mm_internal.h"

// Declared here rather than by including <malloc.h>, which marks several of
// them deprecated and would fail the -Werror build for defining exactly the
// functions this file exists to define.
void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
void *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);
void *__libc_malloc(size_t size);
void __libc_free(void *ptr);
void *__libc_calloc(size_t nmemb, size_t size);
void *__libc_realloc(void *ptr, size_t size);
void *__libc_memalign(size_t alignment, size_t size);

// --- Diagnostics -----------------------------------------------------------
//
// write(2) and nothing else. Every printf-family function allocates, and this
// code runs inside malloc.

static bool g_debug;    // MARS_SHIM_DEBUG
static bool g_check;    // MARS_SHIM_CHECK:   mm_check_heap at exit
static bool g_nofresh;  // MARS_SHIM_NOFRESH: make calloc always pay its memset
static unsigned long long g_flip_at;  // MARS_SHIM_FLIP: see maybe_flip

// Where the diagnostics go. Normally stderr, but a program is entitled to
// close stderr before this library's destructor runs, and several do: gnulib's
// close_stdout atexit handler closes both standard streams, so every coreutils
// program that writes output does. The heap check at exit is exactly the thing
// that must not be lost that way, so MARS_SHIM_CHECK may name a file instead of
// being a flag, and the report is appended to it.
static int g_out = STDERR_FILENO;
static char g_out_path[256];

static void say(const char *msg) {
  ssize_t w = write(g_out, msg, strlen(msg));
  (void)w;  // there is nothing useful to do if the diagnostic itself fails
}

static void say_num(unsigned long long v) {
  char buf[24];
  size_t i = sizeof(buf);
  buf[--i] = '\0';
  if (v == 0) buf[--i] = '0';
  while (v != 0 && i > 0) {
    buf[--i] = (char)('0' + (v % 10));
    v /= 10;
  }
  say(&buf[i]);
}

static void debug(const char *msg) {
  if (g_debug) say(msg);
}

// --- Bootstrap -------------------------------------------------------------
//
// dlsym allocates, and so does the dynamic loader before any constructor in
// this library has run. Everything that happens before the arena is up is
// served from here, and freeing one of these is a no-op: the pool is never
// reused, which costs a few kilobytes once and removes a whole class of
// ordering bug in exchange.

#define BOOTSTRAP_BYTES (64u * 1024u)

static uint8_t g_pool[BOOTSTRAP_BYTES] __attribute__((aligned(16)));
static size_t g_pool_used;
static size_t g_pool_peak;

// Each bootstrap block records its size, so a realloc of one can copy the right
// number of bytes out. Sized to keep the payload 16-aligned.
typedef struct pool_hdr {
  size_t size;
  size_t pad;
} pool_hdr;

static bool from_pool(const void *p) {
  const uint8_t *q = (const uint8_t *)p;
  return q >= g_pool && q < g_pool + BOOTSTRAP_BYTES;
}

static void *pool_alloc(size_t size) {
  size_t need = sizeof(pool_hdr) + ((size + 15u) & ~(size_t)15u);
  if (need < size || g_pool_used + need > BOOTSTRAP_BYTES) {
    // Nothing can be done: the arena does not exist yet and there is no other
    // allocator to fall back on. Say so, rather than returning NULL into a
    // loader that will not check it.
    say("mars shim: bootstrap pool exhausted\n");
    return NULL;
  }
  pool_hdr *h = (pool_hdr *)(void *)(g_pool + g_pool_used);
  h->size = size;
  h->pad = 0;
  g_pool_used += need;
  if (g_pool_used > g_pool_peak) g_pool_peak = g_pool_used;
  return (uint8_t *)(void *)h + sizeof(pool_hdr);
}

static size_t pool_size_of(const void *p) {
  const pool_hdr *h =
      (const pool_hdr *)(const void *)((const uint8_t *)p - sizeof(pool_hdr));
  return h->size;
}

// --- Start-up --------------------------------------------------------------
//
// The order matters, and `g_starting` is what makes the recursion terminate:
// while it is set, every allocation goes to the pool -- including the ones
// dlsym makes on our behalf.

static bool g_ready;
static bool g_starting;
static void (*g_real_free)(void *);

static bool flag(const char *name) {
  const char *v = getenv(name);  // getenv does not allocate
  return v != NULL && v[0] != '\0' && v[0] != '0';
}

static unsigned long long number(const char *name) {
  const char *v = getenv(name);
  if (v == NULL) return 0;
  return strtoull(v, NULL, 10);  // strtoull does not allocate
}

// A copy, not a pointer into the environment: the program owns that memory and
// may reuse it long before this is read at exit.
static void remember_out_path(void) {
  const char *v = getenv("MARS_SHIM_CHECK");
  if (v == NULL || v[0] != '/') return;
  size_t n = strlen(v);
  if (n >= sizeof(g_out_path)) return;
  memcpy(g_out_path, v, n + 1);
}

static void startup(void) {
  if (g_ready || g_starting) return;
  g_starting = true;

  g_debug = flag("MARS_SHIM_DEBUG");
  g_check = flag("MARS_SHIM_CHECK");
  g_nofresh = flag("MARS_SHIM_NOFRESH");
  g_flip_at = number("MARS_SHIM_FLIP");
  remember_out_path();

  // This is the call that allocates. Whatever it needs comes out of the pool,
  // because g_starting is already set.
  g_real_free = (void (*)(void *))(uintptr_t)dlsym(RTLD_NEXT, "free");

  if (mm_arena_init_growable() != 0) {
    say("mars shim: could not map an arena; staying on the bootstrap pool\n");
    g_starting = false;
    return;
  }
  // Raw pointers from here on, so no payload checksum is claimed.
  mm_set_mode(MM_MODE_LIBC);

  g_starting = false;
  g_ready = true;
  debug("mars shim: arena up\n");
}

// Runs before main, which covers the ordinary case. It is not enough on its
// own, because the loader allocates before constructors run, so every entry
// point below still checks. Both are needed; neither is redundant.
__attribute__((constructor)) static void shim_start(void) { startup(); }

__attribute__((destructor)) static void shim_end(void) {
  if (!g_ready) return;

  // Reopen somewhere the report can actually land, if one was named. open(2)
  // does not allocate, which is why this is done here rather than at start-up:
  // holding a descriptor open for the life of the program would change what
  // the program under test sees in its own file-descriptor table.
  if (g_out_path[0] != '\0') {
    int fd = open(g_out_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) g_out = fd;
  }

  if (g_check) {
    // The whole arena, walked block by block, after a real program has finished
    // using it. This is what turns "the program ran" into "the program ran and
    // the heap it left behind is consistent".
    mm_status_t s = mm_check_heap();
    if (s == MM_OK) {
      say("mars shim: heap consistent at exit\n");
    } else {
      say("mars shim: HEAP INCONSISTENT AT EXIT: ");
      say(mm_strerror(s));
      say("\n");
    }
  }
  if (g_debug) {
    mm_stats_t st;
    mm_stats_get(&st);
    say("mars shim: allocs=");
    say_num(st.alloc_calls);
    say(" frees=");
    say_num(st.free_calls);
    say(" reallocs=");
    say_num(st.realloc_calls);
    say(" peak_bytes=");
    say_num((unsigned long long)st.peak_block_bytes);
    say(" quarantined=");
    say_num(st.quarantined_blocks);
    say(" bootstrap_peak=");
    say_num((unsigned long long)g_pool_peak);
    say("\n");
  }
}

// Passes on whatever the allocator ran into, without disturbing the program. A
// shim that swallowed these would have removed the only reason to be running
// this allocator underneath anything.
//
// MM_ERR_NOMEM is not a fault, and MM_ERR_DEGRADED is the mode working as
// designed rather than something to report once per call.
static void report(const char *what) {
  mm_status_t s = mm_last_error();
  if (s == MM_OK || s == MM_ERR_NOMEM || s == MM_ERR_DEGRADED) return;
  say("mars shim: ");
  say(what);
  say(": ");
  say(mm_strerror(s));
  say("\n");
}

// --- Injecting a fault into a real program ---------------------------------
//
// MARS_SHIM_FLIP=<n> flips one bit in the header of the n-th allocation, once.
//
// This exists because the fault injector in tools/ measures the allocator
// against a workload written to be measured, and that is a weaker claim than it
// looks. What a spacecraft allocator has to survive is a bit flipping under
// software that knows nothing about it -- so the same event is staged inside
// `git status` or a Python interpreter, and the allocator either reports it or
// it does not.
//
// The bit chosen is in the second byte of the control word, which carries the
// low bits of the block extent. That is the damage with consequences: an extent
// is what every subsequent walk is positioned by, and getting it wrong is how a
// corrupted header turns into a wild write. Under `hardened` and `paranoid` the
// header checksum catches it; under `fast` there is no checksum, and what
// stands between it and the arena is the bounds check in mm_publish. Both
// outcomes are the point.

static unsigned long long g_allocs_seen;
static bool g_flipped;

static void maybe_flip(void *p) {
  if (g_flip_at == 0 || g_flipped) return;
  if (++g_allocs_seen != g_flip_at) return;
  g_flipped = true;

  uint8_t *hdr = (uint8_t *)p - MM_HDR_SIZE;
  hdr[1] ^= 0x10u;
  say("mars shim: flipped one bit in the header of allocation #");
  say_num(g_allocs_seen);
  say("\n");
}

// --- The malloc family -----------------------------------------------------

void *malloc(size_t size) {
  if (!g_ready) {
    startup();
    if (!g_ready) return pool_alloc(size);
  }
  // C permits malloc(0) to return NULL. Programs assume it does not far more
  // often than the standard entitles them to, and one that treats NULL as
  // failure would abort here. A minimum-size block costs less than the
  // argument does.
  void *p = mm_malloc(size == 0 ? 1 : size);
  if (p == NULL) return NULL;
  report("malloc");
  maybe_flip(p);
  return p;
}

void free(void *ptr) {
  if (ptr == NULL) return;
  // The pool is never reused, so a block from it has no metadata to check and
  // no list to go back on. This is deliberately nothing at all.
  if (from_pool(ptr)) return;
  if (!g_ready) startup();

  if (g_ready && mm_owns(ptr)) {
    mm_free(ptr);
    report("free");
    return;
  }
  // Not ours -- and nothing was dereferenced to find that out. This is the
  // pointer a program obtained before this library was loaded, and it belongs
  // to the allocator that produced it.
  if (g_real_free != NULL) g_real_free(ptr);
}

void *calloc(size_t nmemb, size_t size) {
  size_t total;
  if (__builtin_mul_overflow(nmemb, size, &total)) {
    // The overflow is the attack, not a corner case: a caller that computes
    // the product in size_t and gets a small number allocates a small block
    // and then writes nmemb * size bytes into it.
    return NULL;
  }

  if (!g_ready) {
    startup();
    // The pool lives in .bss and nothing in it is ever reused, so it is zero by
    // construction and needs no memset.
    if (!g_ready) return pool_alloc(total);
  }

  size_t dirty = SIZE_MAX;
  void *p = mm_malloc_fresh(total == 0 ? 1 : total, &dirty);
  if (p == NULL) return NULL;
  report("calloc");

  // Straight from the kernel means already zero, except for the two words the
  // allocator wrote into the free block this was carved from. Skipping the rest
  // is what makes a large calloc cost the mapping and nothing else -- which is
  // what glibc does too, and is why calloc of a big buffer beats
  // malloc-then-memset of one. MARS_SHIM_NOFRESH turns it off so that the
  // difference can be measured rather than asserted.
  if (g_nofresh || dirty == SIZE_MAX) {
    memset(p, 0, total);
  } else if (dirty != 0) {
    memset(p, 0, dirty < total ? dirty : total);
  }
  return p;
}

void *realloc(void *ptr, size_t size) {
  if (ptr == NULL) return malloc(size);

  if (from_pool(ptr)) {
    // Out of the pool and into the arena. The pool block is not freed, because
    // pool blocks never are; a few kilobytes at start-up is the whole cost.
    if (size == 0) return NULL;
    void *fresh = malloc(size);
    if (fresh == NULL) return NULL;
    size_t had = pool_size_of(ptr);
    memcpy(fresh, ptr, had < size ? had : size);
    return fresh;
  }

  if (!g_ready) startup();
  if (g_ready && mm_owns(ptr)) {
    void *p = mm_realloc(ptr, size);
    report("realloc");
    return p;
  }

  // Foreign. There is no way to ask the real allocator how big the block was,
  // so it cannot be copied into the arena: it goes back to the allocator that
  // made it.
  static void *(*real_realloc)(void *, size_t);
  if (real_realloc == NULL) {
    real_realloc =
        (void *(*)(void *, size_t))(uintptr_t)dlsym(RTLD_NEXT, "realloc");
  }
  if (real_realloc != NULL) return real_realloc(ptr, size);
  return NULL;
}

// --- Aligned allocation ----------------------------------------------------

static bool power_of_two(size_t n) { return n != 0 && (n & (n - 1)) == 0; }

void *aligned_alloc(size_t alignment, size_t size) {
  if (!power_of_two(alignment)) return NULL;
  if (!g_ready) {
    startup();
    if (!g_ready) return NULL;  // the pool cannot promise an alignment
  }
  void *p = mm_memalign(alignment, size == 0 ? 1 : size);
  if (p == NULL) return NULL;
  report("aligned_alloc");
  return p;
}

int posix_memalign(void **out, size_t alignment, size_t size) {
  // `out` is not checked for NULL. glibc declares this parameter non-null, so
  // the check is a diagnostic under -Werror rather than a defence, and the
  // real allocator does not make it either.
  //
  // posix_memalign additionally requires a multiple of sizeof(void *).
  if (!power_of_two(alignment) || alignment % sizeof(void *) != 0) {
    return EINVAL;
  }
  void *p = aligned_alloc(alignment, size);
  if (p == NULL) return ENOMEM;
  *out = p;
  return 0;
}

void *memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

static size_t page_size(void) {
  long n = sysconf(_SC_PAGESIZE);
  return n > 0 ? (size_t)n : 4096u;
}

void *valloc(size_t size) { return aligned_alloc(page_size(), size); }

void *pvalloc(size_t size) {
  size_t p = page_size();
  if (size > SIZE_MAX - p) return NULL;
  return aligned_alloc(p, (size + p - 1) & ~(p - 1));
}

// --- Introspection ---------------------------------------------------------

size_t malloc_usable_size(void *ptr) {
  if (ptr == NULL) return 0;
  if (from_pool(ptr)) return pool_size_of(ptr);
  if (!g_ready || !mm_owns(ptr)) return 0;
  // The whole block less its trailer, not what was asked for. A program is
  // entitled to write every byte of the answer, which is exactly why a
  // libc-mode block records that much as its payload -- see finish_allocation
  // in mm_core.c. Reporting the request would understate the block; reporting
  // the block without subtracting the trailer would put the caller's writes on
  // top of the canary.
  return mm_usable_size(ptr);
}

// glibc's own internal names. A few libraries and the C++ runtime call these
// directly, and a program that reached the real malloc through one of them
// would have half its allocations in each allocator.
void *__libc_malloc(size_t size) { return malloc(size); }
void __libc_free(void *ptr) { free(ptr); }
void *__libc_calloc(size_t nmemb, size_t size) { return calloc(nmemb, size); }
void *__libc_realloc(void *ptr, size_t size) { return realloc(ptr, size); }
void *__libc_memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

// --- Threads ---------------------------------------------------------------
//
// Nothing here in the ordinary build. The allocator underneath gives each
// thread its own arena and routes cross-thread frees between them, so a
// threaded program needs no help from the shim and gets none -- interposing
// pthread_create to say "this works" would be a diagnostic nobody asked for on
// every threaded program in existence.
//
// Under `MARS_LOCK=none` it is a different library. There is no locking
// anywhere in it, so a second thread allocating means two threads mutating the
// same bins and the same tiling, and that does not produce a slow program: it
// produces a corrupted one, met as a mysterious crash inside the program under
// test with nothing pointing back here. So that build, and only that build,
// says so once and then gets out of the way. Refusing to create the thread
// would break the program outright; creating it silently would break it in a
// way nobody could diagnose.

#if MM_LOCK == MM_LOCK_NONE
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg) {
  static int (*real_create)(pthread_t *, const pthread_attr_t *,
                            void *(*)(void *), void *);
  static bool warned;

  if (!warned) {
    warned = true;
    say("mars shim: this program creates threads and this build has no "
        "locking (MARS_LOCK=none) -- its heap is not safe under "
        "LD_PRELOAD\n");
  }
  if (real_create == NULL) {
    real_create = (int (*)(pthread_t *, const pthread_attr_t *,
                           void *(*)(void *), void *))(uintptr_t)
        dlsym(RTLD_NEXT, "pthread_create");
  }
  if (real_create == NULL) return ENOSYS;
  return real_create(thread, attr, fn, arg);
}
#endif
