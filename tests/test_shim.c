// The libc surface, tested against the shim itself.
//
// This binary is linked against libmars_preload.so rather than against
// libmars, so every malloc in it -- including the ones the test framework makes
// -- goes through the same code an LD_PRELOAD-ed program would go through. The
// interposition is real; only the mechanism for arranging it differs.
//
// What it cannot test is bootstrap, because by the time a test body runs the
// arena is long since up. That is covered where it actually happens: the
// preload job in .github/workflows/ci.yml runs real programs, and a bootstrap
// failure is not subtle -- nothing starts at all.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

// Declared rather than taken from <malloc.h>, which marks some of these
// deprecated; the shim defines all of them.
void *memalign(size_t alignment, size_t size);
void *valloc(size_t size);
size_t malloc_usable_size(void *ptr);
int posix_memalign(void **out, size_t alignment, size_t size);

// --- The mode --------------------------------------------------------------

MM_TEST(shim, the_arena_is_up_and_in_libc_mode) {
  // Set by the shim's constructor, before anything in this file ran. If this
  // fails, nothing else in the file means anything.
  CHECK_EQ(mm_get_mode(), MM_MODE_LIBC);
  CHECK_TRUE(mm_arena_live());
  CHECK_EQ(mm_check_heap(), MM_OK);
}

// --- malloc and free -------------------------------------------------------

MM_TEST(shim, malloc_returns_aligned_writable_memory) {
  for (size_t n = 1; n <= 4096; n = n * 3 + 1) {
    uint8_t *p = (uint8_t *)malloc(n);
    REQUIRE_NOT_NULL(p);
    CHECK_EQ((uintptr_t)p % mm_alignment(), 0);
    memset(p, 0x5a, n);
    for (size_t i = 0; i < n; i++) CHECK_EQ(p[i], 0x5au);
    free(p);
  }
  CHECK_EQ(mm_check_heap(), MM_OK);
}

MM_TEST(shim, malloc_of_zero_returns_something_freeable) {
  // The standard permits NULL. Far more programs assume a unique pointer than
  // are entitled to, and one that treats NULL as failure would abort.
  void *p = malloc(0);
  CHECK_NOT_NULL(p);
  free(p);
  free(NULL);  // and this must be nothing at all
  CHECK_EQ(mm_check_heap(), MM_OK);
}

MM_TEST(shim, usable_size_is_the_whole_of_what_may_be_written) {
  // The hazard this answers: a program is entitled to write every byte
  // malloc_usable_size reports. If the canary sat inside that, a correct
  // program would be reported as corrupting its own heap.
  for (size_t n = 1; n <= 8192; n = n * 2 + 7) {
    uint8_t *p = (uint8_t *)malloc(n);
    REQUIRE_NOT_NULL(p);
    size_t usable = malloc_usable_size(p);
    CHECK_GE(usable, n);

    memset(p, 0xa5, usable);
    CHECK_EQ(mm_verify(p), MM_ERR_DEGRADED);  // intact; payload not checked
    CHECK_EQ(mm_check_heap(), MM_OK);
    free(p);
    CHECK_EQ(mm_last_error(), MM_OK);
  }
}

MM_TEST(shim, usable_size_disowns_what_it_did_not_hand_out) {
  int on_the_stack = 0;
  static int in_bss;
  CHECK_EQ(malloc_usable_size(NULL), (size_t)0);
  CHECK_EQ(malloc_usable_size(&on_the_stack), (size_t)0);
  CHECK_EQ(malloc_usable_size(&in_bss), (size_t)0);
}

// --- calloc ----------------------------------------------------------------

MM_TEST(shim, calloc_returns_zeroed_memory) {
  static const size_t sizes[] = {1, 64, 4096, 100000, 3u * 1024u * 1024u};
  for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
    uint8_t *p = (uint8_t *)calloc(1, sizes[k]);
    REQUIRE_NOT_NULL(p);
    for (size_t i = 0; i < sizes[k]; i++) {
      if (p[i] != 0) {
        MARS_FAIL_("calloc(%zu) byte %zu was %u", sizes[k], i, p[i]);
        break;
      }
    }
    free(p);
  }
  CHECK_EQ(mm_check_heap(), MM_OK);
}

MM_TEST(shim, calloc_zeroes_memory_that_has_been_used_before) {
  // The interesting case for the fresh-page shortcut: memory that is emphatically
  // not fresh. A calloc that trusted a stale freshness flag would hand back the
  // pattern written below.
  size_t n = 200000;
  uint8_t *dirty = (uint8_t *)malloc(n);
  REQUIRE_NOT_NULL(dirty);
  memset(dirty, 0xcc, n);
  free(dirty);

  uint8_t *p = (uint8_t *)calloc(n, 1);
  REQUIRE_NOT_NULL(p);
  for (size_t i = 0; i < n; i++) {
    if (p[i] != 0) {
      MARS_FAIL_("recycled calloc byte %zu was %u", i, p[i]);
      break;
    }
  }
  free(p);
}

// Through volatiles, so that the arguments reach calloc at run time. Written
// as constants, the compiler folds the product itself, reports it as a
// diagnostic, and the call that was supposed to be tested never happens.
static void *calloc_v(size_t n, size_t size) {
  volatile size_t vn = n;
  volatile size_t vs = size;
  return calloc(vn, vs);
}

MM_TEST(shim, calloc_refuses_a_product_that_overflows) {
  // The overflow is the attack rather than a corner case: a caller that
  // computes the product in size_t and gets a small number allocates a small
  // block and then writes nmemb * size bytes into it.
  CHECK_NULL(calloc_v(SIZE_MAX, 2));
  CHECK_NULL(calloc_v(2, SIZE_MAX));
  CHECK_NULL(calloc_v(SIZE_MAX / 2 + 1, 4));
  CHECK_NULL(calloc_v((size_t)1 << 32, (size_t)1 << 32));
  CHECK_EQ(mm_check_heap(), MM_OK);

  // And a product that does not overflow but cannot be served is a plain
  // failure, not a crash.
  CHECK_NULL(calloc_v(SIZE_MAX / 4, 1));
  CHECK_EQ(mm_check_heap(), MM_OK);
}

// --- realloc ---------------------------------------------------------------

MM_TEST(shim, realloc_preserves_contents_in_both_directions) {
  size_t n = 1000;
  uint8_t *p = (uint8_t *)malloc(n);
  REQUIRE_NOT_NULL(p);
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 31u);

  p = (uint8_t *)realloc(p, 64000);
  REQUIRE_NOT_NULL(p);
  for (size_t i = 0; i < n; i++) CHECK_EQ(p[i], (uint8_t)(i * 31u));

  p = (uint8_t *)realloc(p, 200);
  REQUIRE_NOT_NULL(p);
  for (size_t i = 0; i < 200; i++) CHECK_EQ(p[i], (uint8_t)(i * 31u));

  CHECK_NULL(realloc(p, 0));  // frees, like every libc realloc does
  CHECK_EQ(mm_check_heap(), MM_OK);

  void *fresh = realloc(NULL, 128);  // and this is malloc
  CHECK_NOT_NULL(fresh);
  free(fresh);
}

// --- Aligned allocation ----------------------------------------------------

MM_TEST(shim, aligned_alloc_meets_the_alignment_it_was_given) {
  for (size_t a = 16; a <= 8192; a *= 2) {
    for (size_t n = 1; n <= 5000; n = n * 7 + 3) {
      void *p = aligned_alloc(a, n);
      REQUIRE_NOT_NULL(p);
      CHECK_EQ((uintptr_t)p % a, 0);
      CHECK_GE(malloc_usable_size(p), n);
      memset(p, 1, n);
      free(p);
    }
  }
  CHECK_EQ(mm_check_heap(), MM_OK);
}

MM_TEST(shim, aligned_alloc_rejects_an_alignment_that_is_not_a_power_of_two) {
  CHECK_NULL(aligned_alloc(0, 64));
  CHECK_NULL(aligned_alloc(24, 64));
  CHECK_NULL(aligned_alloc(100, 64));
}

MM_TEST(shim, posix_memalign_and_its_relatives) {
  void *p = NULL;
  CHECK_EQ(posix_memalign(&p, 256, 1000), 0);
  REQUIRE_NOT_NULL(p);
  CHECK_EQ((uintptr_t)p % 256, 0);
  free(p);

  // Not a multiple of sizeof(void *), which posix_memalign requires and
  // aligned_alloc does not.
  void *q = NULL;
  CHECK_NE(posix_memalign(&q, 4, 64), 0);

  void *m = memalign(512, 300);
  REQUIRE_NOT_NULL(m);
  CHECK_EQ((uintptr_t)m % 512, 0);
  free(m);

  void *v = valloc(64);
  REQUIRE_NOT_NULL(v);
  CHECK_EQ((uintptr_t)v % 4096, 0);
  free(v);

  CHECK_EQ(mm_check_heap(), MM_OK);
}

// --- Under load ------------------------------------------------------------

MM_TEST(shim, a_long_churn_leaves_a_consistent_heap) {
  // Enough traffic to make the arena grow past its first chunk and to exercise
  // the patrol, which runs underneath all of this on its own schedule.
  enum { LIVE = 1024 };
  void *live[LIVE] = {0};
  uint64_t x = 0x243F6A8885A308D3ull ^ mars_test_seed();

  for (int step = 0; step < 60000; step++) {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    size_t slot = (size_t)(x % LIVE);
    if (live[slot] != NULL) {
      free(live[slot]);
      live[slot] = NULL;
      continue;
    }
    size_t n = (size_t)(x >> 32) % 16384 + 1;
    live[slot] = (step % 3 == 0) ? calloc(1, n) : malloc(n);
    REQUIRE_NOT_NULL(live[slot]);
    memset(live[slot], (int)slot, n < 32 ? n : 32);
  }

  CHECK_EQ(mm_check_heap(), MM_OK);
  CHECK_GT(g_arena.span_count, (size_t)1);

  for (size_t i = 0; i < LIVE; i++) free(live[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
}

MM_TEST(shim, large_allocations_are_mapped_and_handed_back) {
  size_t before = g_arena.span_count;
  for (int i = 0; i < 32; i++) {
    void *p = malloc(6u * 1024u * 1024u);
    REQUIRE_NOT_NULL(p);
    memset(p, i, 4096);
    free(p);
  }
  // Every one of them was released again, so the arena is no larger than it
  // started. A shim that leaked its mappings would show here and nowhere else.
  CHECK_EQ(g_arena.span_count, before);
  CHECK_EQ(mm_check_heap(), MM_OK);
}
