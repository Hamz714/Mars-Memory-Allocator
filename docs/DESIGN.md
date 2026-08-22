# Design

What this allocator is, how it is put together, and which decisions were forced
by which constraints. [BLOCK_LAYOUT.md](BLOCK_LAYOUT.md) is the authority on
the shape of a block, [FAULT_MODEL.md](FAULT_MODEL.md) on what corruption is
defended against, and [BENCHMARK_METHOD.md](BENCHMARK_METHOD.md) on how the
numbers were taken. This file is the level above all three.

## Validated access is not optional, and that is the central constraint

An allocator that hands back a raw pointer has, at that instant, given up the
ability to say anything about the bytes behind it. The caller can write through
that pointer without telling anyone. Any checksum the allocator holds over the
payload is stale from the next store onwards, and a checksum that is
*sometimes* stale is worse than no checksum: it produces false reports, and a
detector nobody believes is not a detector.

So payload integrity is offered only where it can actually be maintained:

```c
int64_t mm_read (const void *ptr, size_t offset, void *buf, size_t len);
int64_t mm_write(      void *ptr, size_t offset, const void *src, size_t len);
```

`mm_write` refreshes the payload checksum after copying; `mm_read` verifies it
before copying out. Between the two, the allocator knows what the payload is
supposed to contain, and `mm_verify` and the patrol can both check it.

`mm_malloc` still returns an ordinary pointer, and writing through it is
permitted. What that costs is precise and worth stating rather than hiding:

- The **header, canary and boundary tags** are still fully protected. They live
  outside the payload, the allocator is the only writer, and every path that
  touches a block validates them first.
- The **payload checksum** covers only blocks whose contents were established
  through `mm_write`. `payload_crc == 0` means *not established*, and
  `mm_malloc` deliberately leaves it that way — the caller has not written
  anything yet, and establishing it eagerly would make every allocation O(size).
- A block written through its raw pointer keeps whatever payload checksum it
  last had. The patrol therefore **reports** a payload mismatch and leaves the
  block alone, where the read path treats it as corruption. The patrol was not
  asked for those bytes; destroying a live block over a checksum the caller
  never promised to maintain would be the wrong trade.

This is a property of returning pointers at all, not a gap in the
implementation, and no amount of extra metadata removes it. Naming it is the
first thing this document does because every other decision below sits inside
it.

## Architecture

```
include/mars/allocator.h   the public surface: 18 functions, mm_status_t, mm_stats_t
        |
src/mm_layout.h            block shape, control word, accessors -- per profile
src/mm_freelist.{h,c}      bin indexing, the 128-bit bitmap, hardened unlink
src/mm_internal.h          arena state, constants, internal declarations
src/mm_core.c              init, malloc, free, realloc, split, coalesce
src/mm_integrity.c         checksums, canary, validation, recovery, quarantine,
                           mm_check_heap, mm_scrub
src/mm_access.c            mm_read / mm_write
src/mm_crc32.{h,c}         CRC32C: runtime SSE4.2 dispatch, table fallback
src/mm_stats.c             counters, compiled out unless MM_STATS
```

Everything that knows how a block is shaped lives in `mm_layout.h`, and the
rest of the allocator is written against its accessors. That is what makes the
integrity profile a compile-time choice rather than a fork of the allocator:
`-DMARS_PROFILE=fast|hardened|paranoid` changes one header and nothing else.

The arena is caller-supplied and fixed. `mm_init` zeroes it once, so a payload
checksum never covers indeterminate memory, and draws a per-arena secret that
binds every checksum and canary to the arena it was computed in.

## The block, per profile

Sizes are 16-granular and `MM_ALIGNMENT` is 16. It is the **payload** that has
to land on the alignment, not the block, so where the header is not a multiple
of 16 the whole tiling shifts by the difference (`MM_BLOCK_OFFSET`) instead of
padding every block. That is why an 8-byte header costs 8 bytes and not 16.

```
fast -- 8 B of metadata per allocation, no checksum, no canary

  +0    control word (8 B)   size | slack | flags
  +8    payload ..........................................
        slack from rounding up to 16

hardened (default) -- 24 B

  +0    control word (8 B)
  +8    hdr_crc (4 B) | payload_crc (4 B)
  +16   payload ..........................................
  +..   canary (8 B)        bound to the block's index
        slack from rounding up to 16

paranoid -- 40 B

  +0    control word (8 B)
  +8    hdr_crc (4 B) | payload_crc (4 B)
  +16   payload ..........................................
  +..   canary (8 B)
        slack from rounding up to 16
  -16   tail mirror (16 B)  full copy of the header, at a fixed
                            offset from the block's *end*

any profile, when the block is free

  +0            control word
  +MM_HDR_SIZE  next ^ secret, prev ^ secret
  +size-8       boundary tag = block_size
```

| | `fast` | `hardened` | `paranoid` |
|---|---:|---:|---:|
| `MM_HDR_SIZE` | 8 | 16 | 16 |
| canary / mirror | – / – | 8 / – | 8 / 16 |
| metadata per allocation | 8 | 24 | 40 |
| `MM_MIN_BLOCK` | 32 | 48 | 48 |
| max slack | 39 | 63 | 71 |

`requested_size` is not stored. It is recovered as
`block_size - MM_HDR_SIZE - slack`, where `slack` is a 7-bit field whose bound
is derived from the profile's own constants and pinned by a `_Static_assert`,
so a future profile that overflows it fails to compile rather than returning
wrong sizes. Six bits would fit `hardened` exactly and truncate under
`paranoid`; [BLOCK_LAYOUT.md](BLOCK_LAYOUT.md) derives the bound.

## Size classes

Free blocks are filed into **128 bins** with a 128-bit bitmap over them.

| Bins | Scheme | Covers | Search |
|---|---|---|---|
| 0–63 | exact classes, `MM_MIN_BLOCK + 16i` | up to ~1 KB | none — the head of the bin is the answer |
| 64–127 | log-spaced, 4 sub-bins per octave from 2¹⁰ | 1 KB to 2²⁶, top bin catches everything above | capped at `MM_BIN_SCAN_CAP` = 16 |

`mm_bin_of` is **monotonic non-decreasing** in the block size, and every
shortcut in the search is a consequence of that one property: a bin above the
one a request maps to can only hold blocks that are strictly larger, so its
head fits without being examined. The bitmap then finds the smallest non-empty
bin above it with a mask and a count-trailing-zeros, so empty bins cost
nothing. Only the bin the request itself lands in is scanned, and that scan is
capped — a good fit rather than a best fit, which is the deliberate trade: an
unbounded best-fit walk is precisely the cost the bins exist to remove.

Exact bins are kept in LIFO order, because the block freed most recently is the
one most likely to still be in cache.

## Invariants

These are the properties everything else is written against. `mm_check_heap`
checks all of them, and the differential fuzzer runs it continuously.

- **The tiling is the list.** Blocks tile `[lo, hi)` exactly, always. Stepping
  forward is `b + block_size`; stepping back over a free neighbour is its
  boundary tag. There is no adjacency list that can fall out of step with the
  memory it describes.
- **The bins are a cache over the tiling and never the authority.** Anything
  that finds them inconsistent rebuilds from the tiling and reports
  `MM_ERR_CORRUPT_LINKS`.
- **No two adjacent free blocks.** Two of them are a missed coalesce and
  `mm_check_heap` reports it. Coalescing takes both inputs out of their bins
  before their sizes change, and inserts the result into the bin its new size
  asks for.
- **Validate before dereferencing.** Unlinking checks in a fixed order — in the
  arena and aligned, then the header checksum, then free and in *this* bin,
  then both back-links — and only then splices.
- **Never write through an unvalidated block.** A block's control word decides
  where its own trailer lands, so sealing a block whose header failed would
  scatter writes across the arena.
- **Corroborate before writing into a block a walk *found*.** Stronger than the
  line above, and learnt the hard way. `mm_header_ok` establishes that a block is
  what it says only where a checksum backs it; under `fast` it is a bounds check,
  and a run of payload bytes can pass it. So a walk that would write into a block
  because it found one there — the bin rebuild filing free blocks, the
  resynchronisation scan deciding where an abandoned span ends — asks
  `mm_extent_corroborated`, which additionally requires a free block's boundary
  tag to repeat its extent. Every profile has that redundancy; under `fast` it is
  the only redundancy there is.
- **A size that crossed a bin operation is not a size.** Bin operations write
  into free blocks, and one that finds its list damaged rebuilds from the tiling
  and writes into more of them. An extent read before such a call is
  re-established after it (`mm_unchanged`) before any arithmetic uses it.
- **Every surrendered byte is accounted for.** Space the allocator gives up is
  counted in `lost_bytes`, and a quarantined block stays in the tiling as
  permanently-allocated space nobody owns. That turns "space was lost" from an
  inference into an arithmetic identity the consistency check can verify.
- **`mm_last_error` is reset on entry, never on the way out.** A call that
  succeeds after stepping over corruption still reports what it saw, and the
  patrol never clears it.

## Threat model

The fault modelled is a **single-event upset**: ionising radiation flips one or
more bits in DRAM, and a value changes with nothing having written it. Not an
attacker. The per-arena secret makes accidental block confusion detectable; it
is not a security boundary and is not claimed to be one.
[FAULT_MODEL.md](FAULT_MODEL.md) sets out the full taxonomy, what is checked
where, and how the numbers are produced.

Two promises separate cleanly, and they are gated separately in CI:

1. **Never leave the arena.** Every profile promises this unconditionally,
   whatever a corrupted control word says: traversals bounded, extents clamped,
   and `mm_publish` refusing to write through an extent that is not inside the
   arena rather than trusting the paths that reach it. A crash in any
   fault-injection cell fails the build, under all three profiles, and the two
   trials that once broke it are replayed by seed as tests.

   It did not always hold. `fast` broke it twice in 240,000 trials, and the
   cause was that a free block's boundary tag — the only second copy of its
   extent that exists when there is no header checksum — was not being consulted
   by the two walks that write into blocks they find. Detection is the trade
   `fast` makes; the bound was never supposed to be part of that trade.
   [FAULT_MODEL.md](FAULT_MODEL.md#where-that-promise-once-did-not-hold) has the
   full mechanism, which is worth reading as an example of a crash sitting four
   steps downstream of the mistake that caused it.
2. **Detect what the metadata can detect.** This one *depends* on the profile.
   `fast` carries no checksum and no canary, so a flipped header bit is
   undetectable there in principle, and the tests say so explicitly rather than
   leaving a silent gap. Where a checksum exists, the single-bit header cell is
   decidable from theory and CI gates it at 100% detection with zero silent
   corruption.

That second point is worth dwelling on: a cell whose value theory already knows
is worth more than a cell that only has a measurement, because disagreement
between them is a defect rather than a statistic.

## Why detection survived dropping the mirrored footer

The previous layout mirrored the whole 48-byte header at the end of every
block, allocated or free, and spent about 128 bytes of metadata per allocation
doing it. `hardened` sheds 48 of those bytes by keeping no mirror at all, and
sheds no detection guarantee in the process.

The header checksum is CRC32C over 32 bytes: the control word and the payload
checksum — 12 bytes belonging to the block — plus the block's index within the
arena and the per-arena secret. A 32-bit CRC over an input that size detects
**every 1-, 2- and 3-bit error** outright, and misses a random error with
probability 2⁻³². There is nothing left for a second copy to detect.

What the mirror bought was **repair**, not detection: a second copy is what
lets a damaged header be rebuilt rather than surrendered. That is a real
benefit and it is why `paranoid` keeps one. It is simply not the benefit it was
being paid 48 bytes per allocation for under the default profile.

Mixing the index and the secret into the checksum is what turns it from a
bit-flip detector into a **block-confusion** detector. A header copied verbatim
from elsewhere in the same arena — internally consistent, correct in every
field, correct checksum over its own contents — fails, because it was
checksummed for a different position. The canary is bound the same way. Both
have a test that copies one block's metadata over another's and requires the
copy to be rejected.

## Why O(1) allocation required a scrubber

The linear best-fit search that the bins replaced revalidated every free block
on every call. That was its cost — and it was also, incidentally, complete
coverage of every free block in the arena, continuously, for free.

Making allocation O(1) removed the cost and the coverage together. A block
nobody is using is now a block nobody looks at. Shipping that without replacing
the coverage would have traded the entire reliability argument for throughput
and quietly called it a win.

So detection is now two tiers:

- **Validate on touch**, unchanged and O(1): every block popped by `mm_malloc`,
  every block freed, and both coalescing neighbours.
- **`mm_scrub`**, a bounded patrol over the tiling, resuming where it stopped,
  run automatically every `N` allocator calls — 1024 by default, 16 blocks at a
  time, both settable through `mm_set_scrub_interval(ops, budget)`, and `ops`
  of 0 turns it off. This is what a hardware ECC scrubber does, and the
  resemblance is the argument for it rather than a decoration.

The consequence is that **detection latency became a measured quantity** rather
than an implicit one, and it is the reason the scrub interval is a mandatory
column in the fault-injection CSV. The two tiers produce two visibly different
populations: damage to a free header or a free-list link sits on the allocation
path and is found in ten to fifteen calls whatever the patrol is set to, while
damage to an allocated block's header, canary or payload tracks the interval
almost exactly and, with the patrol off, is never found by traffic at all.

[RESULTS.md](RESULTS.md) carries the curve. Turning the patrol off is a real
choice — it trades detection latency for throughput — and the honest way to
offer it is with the price on the label.

## What this is not

- Not thread-safe. There is no lock and no per-thread cache; the arena is
  single-threaded, and none of the published numbers say anything about
  behaviour under concurrency.
- Not a `malloc` replacement. It manages a fixed caller-supplied arena and does
  not grow it; there is no `LD_PRELOAD` shim.
- Not radiation hardening. Real hardening is ECC memory, redundant hardware and
  physical shielding. Nothing in software stops a bit flipping — this can only
  notice afterwards.
- Not able to repair a payload under any profile. If the flip landed in user
  data, the payload checksum reports it and the data is gone.
