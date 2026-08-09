// Corruption detection: checksums, footers, canaries, and quarantine.

#include "mars_test.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mars/allocator.h"
#include "mm_internal.h"

#define ARENA_SIZE (64u * 1024u)

static uint8_t *arena_new(size_t size) { return (uint8_t *)malloc(size); }

// A tiny reproducible generator, so a failure can be replayed from its seed.
static uint64_t rng_state;
static uint64_t next_rand(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

MM_TEST(integrity, a_healthy_block_verifies) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  const char msg[] = "intact";
  REQUIRE_EQ(mm_write(p, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_EQ(mm_verify(p), MM_OK);
  CHECK_EQ(mm_check_heap(), MM_OK);

  mm_free(p);
  free(heap);
}

MM_TEST(integrity, a_flipped_header_bit_is_detected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  mm_header *block = (mm_header *)(void *)((uint8_t *)p - MM_PREFIX);
  block->requested_size ^= 0x40;  // header no longer matches its checksum

  CHECK_NE(mm_verify(p), MM_OK);
  CHECK_NE(mm_check_heap(), MM_OK);

  free(heap);
}

MM_TEST(integrity, a_damaged_footer_is_detected) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  mm_header *block = (mm_header *)(void *)((uint8_t *)p - MM_PREFIX);
  mm_footer *f = mm_footer_of(block);
  f->size ^= 0x10;

  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_HEADER);

  free(heap);
}

MM_TEST(integrity, writing_past_the_payload_trips_the_canary) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(64);
  REQUIRE_NOT_NULL(p);

  // Deliberately overrun by writing straight through the returned pointer,
  // which is exactly what the canary exists to catch.
  memset((uint8_t *)p + 64, 0xFF, 4);

  CHECK_EQ(mm_verify(p), MM_ERR_CORRUPT_CANARY);

  free(heap);
}

MM_TEST(integrity, payload_corruption_is_detected_on_read) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *p = mm_malloc(128);
  REQUIRE_NOT_NULL(p);

  uint8_t data[128];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = (uint8_t)i;
  REQUIRE_EQ(mm_write(p, 0, data, sizeof(data)), 128);

  // Flip a bit behind the allocator's back.
  ((uint8_t *)p)[40] ^= 0x01;

  uint8_t back[128];
  CHECK_EQ(mm_read(p, 0, back, sizeof(back)), -1);
  CHECK_EQ(mm_last_error(), MM_ERR_CORRUPT_PAYLOAD);

  free(heap);
}

MM_TEST(integrity, quarantine_leaves_the_rest_of_the_heap_usable) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);
  REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

  void *a = mm_malloc(128);
  void *victim = mm_malloc(128);
  void *c = mm_malloc(128);
  REQUIRE_NOT_NULL(a);
  REQUIRE_NOT_NULL(victim);
  REQUIRE_NOT_NULL(c);

  const char msg[] = "survivor";
  REQUIRE_EQ(mm_write(a, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));
  REQUIRE_EQ(mm_write(c, 0, msg, sizeof(msg)), (int64_t)sizeof(msg));

  // Wreck the middle block's header, then touch it so the damage is found.
  mm_header *block = (mm_header *)(void *)((uint8_t *)victim - MM_PREFIX);
  block->size ^= 0x20;
  mm_free(victim);
  CHECK_EQ(mm_last_error(), MM_ERR_QUARANTINED);

  // Its neighbours must be unharmed and still readable.
  char back[32] = {0};
  CHECK_EQ(mm_read(a, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);
  memset(back, 0, sizeof(back));
  CHECK_EQ(mm_read(c, 0, back, sizeof(msg)), (int64_t)sizeof(msg));
  CHECK_STR_EQ(back, msg);

  // And the allocator must keep working.
  void *fresh = mm_malloc(64);
  CHECK_NOT_NULL(fresh);

  free(heap);
}

MM_TEST(integrity, scattered_bit_flips_never_crash_the_allocator) {
  uint8_t *heap = arena_new(ARENA_SIZE);
  REQUIRE_NOT_NULL(heap);

  rng_state = mars_test_seed() ? mars_test_seed() : 0x9E3779B97F4A7C15ULL;

  for (int round = 0; round < 40; round++) {
    REQUIRE_EQ(mm_init(heap, ARENA_SIZE), 0);

    enum { N = 16 };
    void *p[N];
    int live = 0;
    for (int i = 0; i < N; i++) {
      p[i] = mm_malloc(64 + (size_t)(i * 16));
      if (p[i] != NULL) {
        uint8_t v = (uint8_t)i;
        mm_write(p[i], 0, &v, 1);
        live++;
      }
    }
    REQUIRE_TRUE(live > 0);

    // Spray bit flips across the whole arena.
    for (int f = 0; f < 24; f++) {
      size_t pos = (size_t)(next_rand() % ARENA_SIZE);
      heap[pos] ^= (uint8_t)(1u << (next_rand() % 8));
    }

    // Every one of these must either succeed or report an error. What must not
    // happen is a crash, a hang, or a wild write -- which is why this test is
    // run under ASan and Valgrind as well as on its own.
    for (int i = 0; i < N; i++) {
      if (p[i] == NULL) continue;
      uint8_t back[256];
      (void)mm_verify(p[i]);
      (void)mm_read(p[i], 0, back, 64);
      (void)mm_write(p[i], 0, back, 8);
    }
    (void)mm_check_heap();
    for (int i = 0; i < N; i++) {
      if (p[i] != NULL) mm_free(p[i]);
    }
    (void)mm_malloc(128);
  }

  free(heap);
}
