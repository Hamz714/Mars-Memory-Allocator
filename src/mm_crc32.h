// CRC32C (Castagnoli) with a runtime-dispatched hardware path.
//
// Chosen over the FNV-1a this replaces because its error-detection properties
// are provable rather than merely plausible: over a message shorter than the
// polynomial's Hamming-distance bound, a 32-bit CRC detects every 1-, 2- and
// 3-bit error outright, and misses a random error with probability 2^-32. That
// guarantee is what pays for dropping the mirrored footer from allocated
// blocks -- the redundancy the mirror provided was detection, and the CRC
// already covers that ground.
//
// The reflected form is used throughout, because that is what the SSE4.2
// CRC32 instruction computes. Software and hardware must agree bit for bit or
// fault-injection results stop being comparable between machines, so both are
// expressed as the same running update over the same state and pinned by a
// test.

#ifndef MARS_MM_CRC32_H_
#define MARS_MM_CRC32_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// One-shot CRC32C: initial state all-ones, final state complemented. This is
// the value external tools mean by "CRC32C", and tests/test_crc32.c pins it
// against the standard "123456789" check vector, 0xE3069283.
uint32_t mm_crc32c(const void *data, size_t len);

// Running update over the raw register state, with no initial or final
// complement. Composable, and the form in which the two implementations are
// compared. Dispatches to hardware where the CPU offers it.
uint32_t mm_crc32c_update(uint32_t crc, const void *data, size_t len);

// The two implementations, reachable directly so a test can assert they agree.
// mm_crc32c_hw is only meaningful when mm_crc32c_have_hw() is true; called
// otherwise it falls through to the software path rather than executing an
// unsupported instruction.
uint32_t mm_crc32c_sw(uint32_t crc, const void *data, size_t len);
uint32_t mm_crc32c_hw(uint32_t crc, const void *data, size_t len);

// Whether this CPU has the SSE4.2 CRC32 instruction and the build can reach
// it. False on non-x86 targets, where the software path is the only path.
bool mm_crc32c_have_hw(void);

#endif  // MARS_MM_CRC32_H_
