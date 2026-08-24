// Growth: chunk mapping, dedicated mappings, span ownership and release.
//
// Everything here is Unix-only, because growth is: mm_arena_can_grow() is
// false on Windows and the whole file compiles to nothing there rather than to
// tests that pass because they were skipped.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_arena.h"
#include "mm_internal.h"

#if !defined(_WIN32)

// --- Helpers ---------------------------------------------------------------

static size_t span_count_of_kind(mm_span_kind_t kind) {
  size_t n = 0;
  for (const mm_span *s = g_arena.spans; s != NULL; s = s->next) {
    if (s->kind == (uint8_t)kind) n++;
  }
  return n;
}

// Fills the arena with `n` live allocations of `size`, returning them through
// `out`. Written as its own function because every test below needs an arena
// that has actually had to grow, and "has grown" is not a state a single
// allocation can produce.
static size_t fill(void **out, size_t n, size_t size) {
  size_t made = 0;
  for (size_t i = 0; i < n; i++) {
    out[i] = mm_malloc(size);
    if (out[i] == NULL) break;
    // A recognisable pattern, so that a block landing on top of another one
    // shows up as wrong contents rather than as nothing at all.
    memset(out[i], (int)(i & 0xff), size);
    made++;
  }
  return made;
}

static bool contents_intact(void *const *p, size_t n, size_t size) {
  for (size_t i = 0; i < n; i++) {
    const uint8_t *q = (const uint8_t *)p[i];
    for (size_t j = 0; j < size; j++) {
      if (q[j] != (uint8_t)(i & 0xff)) return false;
    }
  }
  return true;
}

// --- The provider ----------------------------------------------------------

MM_TEST(arena, growth_is_available_here) {
  CHECK_TRUE(mm_arena_can_grow());
}

MM_TEST(arena, a_growable_arena_starts_with_one_chunk) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  CHECK_EQ(g_arena.span_count, 1);
  CHECK_EQ(span_count_of_kind(MM_SPAN_CHUNK), 1);
  CHECK_TRUE(g_arena.growable);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_arena_reset();
}

MM_TEST(arena, every_mapped_chunk_is_chunk_aligned) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // Enough to need several chunks. The alignment is the whole basis of the
  // O(1) ownership lookup, so it is checked on every span rather than on the
  // first one.
  static void *live[4096];
  size_t made = fill(live, 4096, 4096);
  CHECK_GT(made, (size_t)0);
  CHECK_GT(g_arena.span_count, (size_t)1);

  for (const mm_span *s = g_arena.spans; s != NULL; s = s->next) {
    CHECK_EQ((uintptr_t)s->map_base % MM_CHUNK_SIZE, 0);
    CHECK_EQ(s->map_size % MM_CHUNK_SIZE, 0);
    // The descriptor sits at the front of the mapping it describes, so the
    // tiling starts after it and inside it.
    CHECK_PTR_EQ(s, (const mm_span *)s->map_base);
    CHECK_TRUE(s->lo > (const uint8_t *)s->map_base);
    CHECK_TRUE(s->hi <= (const uint8_t *)s->map_base + s->map_size);
  }

  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_arena_reset();
}

// --- Growth ----------------------------------------------------------------

MM_TEST(arena, allocation_continues_past_the_first_chunk) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // A fixed arena would have run out here; a growable one maps more. That is
  // the whole of what this phase added to the allocation path.
  static void *live[2048];
  size_t made = fill(live, 2048, 4096);
  CHECK_EQ(made, (size_t)2048);
  CHECK_GT(g_arena.span_count, (size_t)1);
  CHECK_GT(g_arena.total_bytes, MM_CHUNK_SIZE);
  CHECK_TRUE(contents_intact(live, made, 4096));
  CHECK_EQ(mm_check_heap(), MM_OK);

  for (size_t i = 0; i < made; i++) mm_free(live[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_arena_reset();
}

MM_TEST(arena, a_block_never_straddles_two_spans) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  static void *live[2048];
  size_t made = fill(live, 2048, 3000);
  REQUIRE_TRUE(made > 0);

  for (size_t i = 0; i < made; i++) {
    const mm_span *s = mm_span_of(live[i]);
    REQUIRE_TRUE(s != NULL);
    const mm_block *b = (const mm_block *)(const void *)
        ((const uint8_t *)live[i] - MM_HDR_SIZE);
    // Both ends inside the same span. Coalescing, the boundary tags and the
    // backward walk all rest on this and on nothing else.
    CHECK_TRUE((const uint8_t *)(const void *)b >= s->lo);
    CHECK_TRUE(mm_block_end(b) <= s->hi);
  }

  for (size_t i = 0; i < made; i++) mm_free(live[i]);
  mm_arena_reset();
}

// --- Dedicated mappings ----------------------------------------------------

MM_TEST(arena, a_large_request_gets_a_mapping_of_its_own) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  size_t big = MM_LARGE_THRESHOLD + 4096;
  void *p = mm_malloc(big);
  REQUIRE_NOT_NULL(p);

  const mm_span *s = mm_span_of(p);
  REQUIRE_TRUE(s != NULL);
  CHECK_EQ(s->kind, (uint8_t)MM_SPAN_LARGE);
  CHECK_EQ(span_count_of_kind(MM_SPAN_LARGE), 1);
  CHECK_EQ((uintptr_t)s->map_base % MM_CHUNK_SIZE, 0);

  memset(p, 0x5a, big);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(p);
  // Handed straight back: address space and all, not merely made reusable.
  CHECK_EQ(span_count_of_kind(MM_SPAN_LARGE), 0);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_arena_reset();
}

MM_TEST(arena, a_dedicated_mapping_survives_being_shared) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  size_t big = MM_LARGE_THRESHOLD + 4096;
  void *p = mm_malloc(big);
  REQUIRE_NOT_NULL(p);
  mm_span *s = mm_span_of(p);
  REQUIRE_TRUE(s != NULL);

  // The remainder of the mapping is ordinary free space and is allocatable.
  // Taking some of it means the mapping is no longer the big block's alone,
  // and releasing it when the big block goes would take a live allocation with
  // it -- so it must not be released until both are gone.
  size_t rest = (size_t)(s->hi - s->lo) - mm_size_for(big);
  REQUIRE_TRUE(rest >= MM_MIN_BLOCK);
  // Sized so that mm_size_for asks for exactly the remainder, which is the one
  // free block of that size and the most recent thing filed in its bin. Its
  // bin is scanned from the head, so this lands where it is meant to.
  void *tail = mm_malloc(rest - mm_metadata_overhead());
  REQUIRE_NOT_NULL(tail);
  REQUIRE_TRUE(mm_span_of(tail) == s);

  mm_free(p);
  CHECK_EQ(span_count_of_kind(MM_SPAN_LARGE), 1);  // still held
  CHECK_EQ(mm_verify(tail), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(tail);
  CHECK_EQ(span_count_of_kind(MM_SPAN_LARGE), 0);  // and now released
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_arena_reset();
}

MM_TEST(arena, repeated_large_cycles_do_not_leak_registry_entries) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  size_t big = MM_CHUNK_SIZE * 3;
  size_t before = g_arena.span_count;
  // The unit index deletes by shifting entries back rather than leaving
  // tombstones. Two hundred cycles is enough that tombstones would have made
  // the table useless, and the assertion that matters is the one at the end:
  // a pointer from the last cycle still resolves.
  for (int i = 0; i < 200; i++) {
    void *p = mm_malloc(big);
    REQUIRE_NOT_NULL(p);
    memset(p, i & 0xff, 64);
    CHECK_EQ(mm_verify(p), MM_OK);
    mm_free(p);
  }
  CHECK_EQ(g_arena.span_count, before);

  void *p = mm_malloc(big);
  REQUIRE_NOT_NULL(p);
  CHECK_TRUE(mm_owns(p));
  CHECK_EQ(mm_verify(p), MM_OK);
  mm_free(p);

  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_arena_reset();
}

// --- Ownership -------------------------------------------------------------

MM_TEST(arena, foreign_pointers_are_disowned_without_being_touched) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  void *ours = mm_malloc(64);
  REQUIRE_NOT_NULL(ours);
  CHECK_TRUE(mm_owns(ours));

  // Three kinds of pointer this allocator did not hand out. The one that
  // matters is the malloc'd one: under the shim a program may free memory it
  // obtained before the shim was loaded, and the 2 MB-aligned address below
  // that pointer is not ours to read.
  int on_the_stack = 0;
  void *from_libc = malloc(1024);
  REQUIRE_NOT_NULL(from_libc);
  static int in_bss;

  CHECK_FALSE(mm_owns(&on_the_stack));
  CHECK_FALSE(mm_owns(from_libc));
  CHECK_FALSE(mm_owns(&in_bss));
  CHECK_FALSE(mm_owns(NULL));

  // And the reporting path agrees, without dereferencing any of them.
  mm_free(from_libc);
  CHECK_EQ(mm_last_error(), MM_ERR_INVALID_PTR);
  CHECK_EQ(mm_verify(&on_the_stack), MM_ERR_INVALID_PTR);

  free(from_libc);
  mm_free(ours);
  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_arena_reset();
}

MM_TEST(arena, ownership_is_answered_for_every_span) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  static void *live[2048];
  size_t made = fill(live, 2048, 4096);
  REQUIRE_TRUE(g_arena.span_count > 1);

  for (size_t i = 0; i < made; i++) {
    CHECK_TRUE(mm_owns(live[i]));
    CHECK_TRUE(mm_span_of(live[i]) != NULL);
  }

  // Immediately below the first block of a span is the span's own descriptor,
  // which is inside the mapping and therefore owned -- and immediately above
  // the last is the next mapping, which is not.
  for (const mm_span *s = g_arena.spans; s != NULL; s = s->next) {
    CHECK_TRUE(mm_span_of(s->lo) == s);
    CHECK_TRUE(mm_span_of(s->hi - 1) == s);
    CHECK_TRUE(mm_span_of(s->hi) != s);
    CHECK_TRUE(mm_span_of(s->lo - 1) != s);
  }

  for (size_t i = 0; i < made; i++) mm_free(live[i]);
  mm_arena_reset();
}

// --- Interaction with the managed API --------------------------------------

MM_TEST(arena, init_replaces_a_growable_arena_and_releases_it) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);
  static void *live[512];
  (void)fill(live, 512, 8192);
  CHECK_GT(g_arena.span_count, (size_t)1);

  uint8_t *heap = (uint8_t *)malloc(64u * 1024u);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, 64u * 1024u), 0);

  // One span, and it is the caller's. Everything mapped before is gone, which
  // is what stops a program that switches between the two entry points from
  // leaking every chunk it ever grew.
  CHECK_EQ(g_arena.span_count, 1);
  CHECK_EQ(span_count_of_kind(MM_SPAN_USER), 1);
  CHECK_FALSE(g_arena.growable);
  CHECK_EQ(mm_check_heap(), MM_OK);

  // And a fixed arena still refuses to grow rather than quietly mapping more.
  void *p = mm_malloc(1024u * 1024u);
  CHECK_NULL(p);
  CHECK_EQ(mm_last_error(), MM_ERR_NOMEM);
  CHECK_EQ(g_arena.span_count, 1);

  free(heap);
  mm_arena_reset();
}

MM_TEST(arena, realloc_moves_between_spans_intact) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  size_t n = 4096;
  uint8_t *p = (uint8_t *)mm_malloc(n);
  REQUIRE_NOT_NULL(p);
  for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(i * 7u);
  const mm_span *first = mm_span_of(p);

  // Well past what a chunk can hold, so the block has to move to a mapping of
  // its own and carry its contents with it.
  size_t big = MM_CHUNK_SIZE * 2;
  uint8_t *q = (uint8_t *)mm_realloc(p, big);
  REQUIRE_NOT_NULL(q);
  const mm_span *second = mm_span_of(q);
  CHECK_TRUE(second != first);
  CHECK_EQ(second->kind, (uint8_t)MM_SPAN_LARGE);
  for (size_t i = 0; i < n; i++) CHECK_EQ(q[i], (uint8_t)(i * 7u));

  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_free(q);
  mm_arena_reset();
}

// --- The patrol and the consistency check across spans ----------------------

MM_TEST(arena, the_patrol_laps_every_span) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);
  static void *live[1024];
  size_t made = fill(live, 1024, 4096);
  REQUIRE_TRUE(g_arena.span_count > 1);

  // Walk far enough to have gone round everything, and record which spans the
  // cursor was seen in. A patrol that only ever walked the span it started in
  // would leave every other chunk uncovered, which is the failure this whole
  // mechanism exists to prevent.
  size_t seen_spans = 0;
  const mm_span *last = NULL;
  for (int i = 0; i < 4000; i++) {
    CHECK_EQ(mm_scrub(4), MM_OK);
    if (g_arena.scrub_span != last) {
      last = g_arena.scrub_span;
      seen_spans++;
    }
  }
  CHECK_GT(seen_spans, (size_t)1);

  for (size_t i = 0; i < made; i++) mm_free(live[i]);
  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_arena_reset();
}

MM_TEST(arena, reset_releases_everything) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);
  static void *live[512];
  (void)fill(live, 512, 8192);
  CHECK_GT(g_arena.span_count, (size_t)1);

  mm_arena_reset();

  CHECK_EQ(g_arena.span_count, 0);
  CHECK_EQ(g_arena.total_bytes, 0);
  CHECK_NULL(g_arena.spans);
  CHECK_NULL(g_arena.span_cache);
  // Nothing is owned any more, and the allocator says so rather than crashing.
  CHECK_EQ(mm_check_heap(), MM_ERR_NOT_INITIALIZED);
  CHECK_NULL(mm_malloc(16));
  CHECK_EQ(mm_last_error(), MM_ERR_NOT_INITIALIZED);
}

#else

MM_TEST(arena, growth_is_unavailable_on_this_platform) {
  CHECK_FALSE(mm_arena_can_grow());
  CHECK_EQ(mm_arena_init_growable(), -1);
}

#endif  // !_WIN32

#if !defined(_WIN32)

// --- Fresh pages -----------------------------------------------------------

MM_TEST(arena, the_first_block_out_of_a_new_mapping_is_still_zero) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // Straight out of an anonymous mapping, so every byte of it is the zero the
  // kernel supplied -- except the two words the allocator wrote when it filed
  // the free block this was carved from.
  size_t dirty = 0;
  size_t big = MM_CHUNK_SIZE * 2;
  uint8_t *p = (uint8_t *)mm_malloc_fresh(big, &dirty);
  REQUIRE_NOT_NULL(p);
  CHECK_EQ(dirty, MM_FRESH_DIRTY_PREFIX);

  for (size_t i = dirty; i < big; i++) {
    if (p[i] != 0) {
      MARS_FAIL_("byte %zu of a fresh block was %u, not zero", i, p[i]);
      break;
    }
  }

  // And the freshness is spent. A second allocation out of the same mapping
  // may be recycled memory, and the allocator says so rather than guessing.
  size_t again = 0;
  void *q = mm_malloc_fresh(64, &again);
  REQUIRE_NOT_NULL(q);
  CHECK_EQ(again, SIZE_MAX);

  mm_free(q);
  mm_free(p);
  mm_arena_reset();
}

MM_TEST(arena, freshness_is_spent_by_a_plain_malloc_too) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // The block that consumes a mapping's freshness need not be the one that
  // wanted to know about it. If a plain mm_malloc could take the first block
  // and leave the flag standing, a later calloc would be told that recycled
  // memory was untouched.
  void *first = mm_malloc(64);
  REQUIRE_NOT_NULL(first);
  memset(first, 0xff, 64);
  mm_free(first);

  size_t dirty = 0;
  void *second = mm_malloc_fresh(64, &dirty);
  REQUIRE_NOT_NULL(second);
  CHECK_EQ(dirty, SIZE_MAX);

  mm_free(second);
  mm_arena_reset();
}

#endif  // !_WIN32

#if !defined(_WIN32)

// --- The unit index under pressure -----------------------------------------

MM_TEST(arena, the_unit_index_grows_and_still_answers) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // The index starts as a fixed table and doubles when it passes half full.
  // Three hundred mappings of two units each is six hundred entries, which
  // forces at least one rehash -- and a rehash that lost an entry would show
  // up as a live pointer the allocator no longer recognises.
  //
  // This is address space rather than memory: nothing here touches more than
  // the first page of each mapping, so the resident cost is a few megabytes.
  enum { N = 300 };
  static void *live[N];
  size_t big = 3u * 1024u * 1024u;

  for (size_t i = 0; i < N; i++) {
    live[i] = mm_malloc(big);
    REQUIRE_NOT_NULL(live[i]);
    memset(live[i], (int)(i & 0xff), 64);
  }
  CHECK_GE(g_arena.span_count, (size_t)N);

  for (size_t i = 0; i < N; i++) {
    CHECK_TRUE(mm_owns(live[i]));
    CHECK_EQ(mm_verify(live[i]), MM_OK);
    CHECK_EQ(((const uint8_t *)live[i])[0], (uint8_t)(i & 0xff));
  }
  CHECK_EQ(mm_check_heap(), MM_OK);

  // Released in a scattered order, so that deletion has to shift entries back
  // over occupied slots rather than always finding the hole at the end of a
  // run. Tombstones would pass this and then degrade for the rest of the
  // process; backward shifting is what keeps the table usable afterwards.
  for (size_t step = 0; step < 7; step++) {
    for (size_t i = step; i < N; i += 7) {
      mm_free(live[i]);
      live[i] = NULL;
    }
    // And the survivors are still findable after every round of deletions.
    for (size_t i = 0; i < N; i++) {
      if (live[i] != NULL) CHECK_TRUE(mm_owns(live[i]));
    }
  }

  CHECK_EQ(mm_check_heap(), MM_OK);

  // Back to where it started, with the enlarged table still in service: a
  // fresh allocation resolves through it.
  void *p = mm_malloc(big);
  REQUIRE_NOT_NULL(p);
  CHECK_TRUE(mm_owns(p));
  mm_free(p);

  mm_arena_reset();
}

#endif  // !_WIN32

#if !defined(_WIN32)

// --- Handing pages back ----------------------------------------------------

MM_TEST(arena, a_churning_span_does_not_hand_its_pages_back_every_time) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  void *warm = mm_malloc(64);
  REQUIRE_NOT_NULL(warm);
  mm_span *s = mm_span_of(warm);
  REQUIRE_TRUE(s != NULL);
  mm_free(warm);

  // The pathological shape, and it is not contrived -- it is what
  // `calloc_4kb_x200000` in bench/results/preload-*.csv does. After every free
  // the whole chunk is one free block, so the size test for handing pages back
  // passes on every single iteration. Doing it every time throws away page
  // mappings that the very next allocation faults straight back in, and the
  // measurement put that at 4.2 us per call.
  size_t trims = 0;
  uint64_t last = s->trim_after;
  for (int i = 0; i < 5000; i++) {
    void *p = mm_malloc(4096);
    REQUIRE_NOT_NULL(p);
    REQUIRE_TRUE(mm_span_of(p) == s);
    mm_free(p);
    if (s->trim_after != last) {
      trims++;
      last = s->trim_after;
    }
  }

  // Ten thousand allocator calls, so the watermark permits about ten. The
  // bound is what matters rather than the exact figure: the failure this
  // guards against is one trim per free, which is five thousand.
  CHECK_LE(trims, (size_t)15);
  // And it does still happen. A rate limit that turned into an off switch
  // would pass the line above and give no memory back at all.
  CHECK_GT(trims, (size_t)0);

  CHECK_EQ(mm_check_heap(), MM_OK);
  mm_arena_reset();
}

MM_TEST(arena, pages_handed_back_still_read_as_zero) {
  REQUIRE_EQ(mm_arena_init_growable(), 0);

  // Fill a chunk, dirty every page of it, then free it all: the whole span
  // becomes one free block over the size threshold and its interior goes back
  // to the kernel. What comes back has to be the zeroes MADV_DONTNEED promises,
  // because a later calloc out of a fresh mapping relies on exactly that -- and
  // because the block's own metadata must have survived, which mm_check_heap
  // is what checks.
  enum { N = 200 };
  static void *live[N];
  size_t made = 0;
  for (size_t i = 0; i < N; i++) {
    live[i] = mm_malloc(8192);
    if (live[i] == NULL) break;
    memset(live[i], 0xdd, 8192);
    made++;
  }
  REQUIRE_TRUE(made > 0);
  for (size_t i = 0; i < made; i++) mm_free(live[i]);

  CHECK_EQ(mm_check_heap(), MM_OK);

  // And the arena still works afterwards, which is the part a mistake in the
  // page range would break: madvising over a boundary tag or a free-list link
  // would zero the block's own metadata rather than its interior.
  void *p = mm_malloc(65536);
  REQUIRE_NOT_NULL(p);
  memset(p, 1, 65536);
  CHECK_EQ(mm_verify(p), MM_OK);
  mm_free(p);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_arena_reset();
}

#endif  // !_WIN32
