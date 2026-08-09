// xoshiro256++, seeded through splitmix64.
//
// Deliberately not rand(): its sequence differs between C libraries, so a seed
// that reproduces a failure on one machine would not reproduce it on another.
// Everything here is defined by the standard's integer semantics alone.

#ifndef MARS_RNG_H_
#define MARS_RNG_H_

#include <stdint.h>

typedef struct mars_rng {
  uint64_t s[4];
} mars_rng;

static inline uint64_t mars_rotl(uint64_t x, int k) {
  return (x << k) | (x >> (64 - k));
}

static inline void mars_rng_seed(mars_rng *r, uint64_t seed) {
  for (int i = 0; i < 4; i++) {
    seed += 0x9E3779B97F4A7C15ULL;
    uint64_t z = seed;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    r->s[i] = z ^ (z >> 31);
  }
}

static inline uint64_t mars_rng_next(mars_rng *r) {
  uint64_t result = mars_rotl(r->s[0] + r->s[3], 23) + r->s[0];
  uint64_t t = r->s[1] << 17;
  r->s[2] ^= r->s[0];
  r->s[3] ^= r->s[1];
  r->s[1] ^= r->s[2];
  r->s[0] ^= r->s[3];
  r->s[2] ^= t;
  r->s[3] = mars_rotl(r->s[3], 45);
  return result;
}

// Uniform in [0, n). Returns 0 when n is 0.
static inline uint64_t mars_rng_below(mars_rng *r, uint64_t n) {
  return n == 0 ? 0 : mars_rng_next(r) % n;
}

#endif  // MARS_RNG_H_
