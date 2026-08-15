// CRC32C: the standard check vector, and hardware against software.
//
// The second test is the one that matters for this project. Fault-injection
// results are only comparable between machines if every machine computes the
// same checksum over the same header, and the SSE4.2 path and the table path
// are two separate implementations of that promise. Without this test a
// mismatch would show up as a quietly different detection rate rather than as
// a failure.

#include "mars_test.h"

#include <stdint.h>
#include <string.h>

#include "mars_rng.h"
#include "mm_crc32.h"

MM_TEST(crc32, matches_the_standard_check_vector) {
  // The Castagnoli check value everyone publishes: CRC32C("123456789").
  CHECK_EQ(mm_crc32c("123456789", 9), 0xE3069283u);

  // Zero-length input leaves the initial state untouched, so the one-shot form
  // comes back as the complement of all-ones.
  CHECK_EQ(mm_crc32c("", 0), 0u);
}

MM_TEST(crc32, is_sensitive_to_order_and_to_every_bit) {
  CHECK_NE(mm_crc32c("ab", 2), mm_crc32c("ba", 2));

  // Every single-bit change in a 12-byte buffer -- the size of the metadata
  // the header CRC covers -- must change the checksum. This is the property
  // the layout leans on when it drops the mirrored footer.
  uint8_t buf[12];
  for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)(i * 7 + 1);
  uint32_t base = mm_crc32c(buf, sizeof(buf));

  for (size_t i = 0; i < sizeof(buf); i++) {
    for (unsigned b = 0; b < 8; b++) {
      buf[i] ^= (uint8_t)(1u << b);
      if (mm_crc32c(buf, sizeof(buf)) == base) {
        MARS_FAIL_("flipping bit %u of byte %zu left the CRC unchanged", b, i);
      }
      buf[i] ^= (uint8_t)(1u << b);
    }
  }
}

MM_TEST(crc32, software_and_hardware_agree) {
  if (!mm_crc32c_have_hw()) {
    // Not a silent pass: say so, because on a machine without SSE4.2 this test
    // proves nothing and the reader needs to know that.
    fprintf(stderr, "    no SSE4.2 on this CPU; only the software path ran\n");
  }

  mars_rng rng;
  mars_rng_seed(&rng, mars_test_seed() ? mars_test_seed() : 0xC0FFEEULL);

  // Lengths from 0 to 63 exercise the eight-byte body and every possible
  // trailing remainder; a few larger buffers exercise the loop itself.
  uint8_t buf[512];
  for (int trial = 0; trial < 4000; trial++) {
    size_t len = (trial < 2000) ? (size_t)(trial % 64)
                                : (size_t)mars_rng_below(&rng, sizeof(buf) + 1);
    for (size_t i = 0; i < len; i++) {
      buf[i] = (uint8_t)(mars_rng_next(&rng) & 0xFFu);
    }
    uint32_t seed = (uint32_t)mars_rng_next(&rng);

    uint32_t sw = mm_crc32c_sw(seed, buf, len);
    uint32_t hw = mm_crc32c_hw(seed, buf, len);
    if (sw != hw) {
      MARS_FAIL_("len %zu seed %08X: software %08X, hardware %08X", len,
                 seed, sw, hw);
      return;
    }
    // And the dispatcher must land on whichever of the two is available.
    CHECK_EQ(mm_crc32c_update(seed, buf, len), sw);
  }
}

MM_TEST(crc32, the_running_form_composes) {
  const char msg[] = "the quick brown fox jumps over the lazy dog";
  size_t n = sizeof(msg) - 1;

  // Splitting the input at every point must give the same running state as
  // feeding it whole. This is what lets the header CRC be built up field by
  // field rather than out of a packed copy.
  uint32_t whole = mm_crc32c_update(0xFFFFFFFFu, msg, n);
  for (size_t cut = 0; cut <= n; cut++) {
    uint32_t part = mm_crc32c_update(0xFFFFFFFFu, msg, cut);
    part = mm_crc32c_update(part, msg + cut, n - cut);
    if (part != whole) {
      MARS_FAIL_("splitting at %zu gave %08X, not %08X", cut, part, whole);
      return;
    }
  }
}
