# Block layout

How a block is shaped, why it is shaped that way, and what each integrity
profile costs and detects.

The previous layout spent about **128 bytes of metadata on every allocation**.
A 64-byte payload therefore ran at 33% utilisation: two thirds of the arena was
bookkeeping. This describes what replaced it.

| Profile | Header | Trailer | Metadata | + avg 16-align rounding | Avg total |
|---|---:|---:|---:|---:|---:|
| `fast` | 8 | 0 | **8** | 8 | 16 |
| `hardened` (default) | 16 | 8 | **24** | 8 | 32 |
| `paranoid` | 16 | 24 | **40** | 8 | 48 |

Selected at configure time with `-DMARS_PROFILE=`. The figures are not
aspirations: `tests/test_layout.c` allocates 64-byte blocks, reads
`mm_stats_t`, and asserts `(peak_block_bytes - peak_payload_bytes) /
peak_blocks` comes out at 16, 32 and 48 respectively.

## Three eliminations

### The footer on allocated blocks — 48 bytes

The old layout mirrored the entire header at the end of every block, allocated
or free. Boundary tags put a footer only in a **free** block, inside payload
space it is not using, and a `PREV_IN_USE` bit in the following header says
whether looking backwards is legal at all. Backward coalescing stays O(1) and
an allocated block pays nothing for it.

This is dlmalloc's trick, and it is the single largest saving here.

### The clue — 8 bytes

An 8-byte value sat immediately below every payload, recording the distance
back to the header, so that a pointer handed in by a caller could be
cross-checked before its header was trusted. With a compile-time-constant
header size and structural 16-byte alignment the header is at
`ptr - MM_HDR_SIZE` unconditionally, and the checksum does the cross-checking
far better than a constant compared against itself.

### The padding — ~8 bytes

The old header was 48 bytes and the prefix was padded to 64 so that payloads
landed on 16. Now the payload alignment is structural: block sizes are
16-granular, the header is a constant, and where the header is *not* a multiple
of 16 the whole tiling is shifted by the difference instead
(`MM_BLOCK_OFFSET`). That is why an 8-byte header costs 8 bytes of overhead and
not 16 — the first block starts 8 bytes into the arena, and every payload after
it is aligned by construction.

## The control word

Every profile starts a block with the same 64-bit word.

```
bits 63..10   block_size >> 4      the block's whole extent, 16-granular
bits  9.. 3   slack                block_size - MM_HDR_SIZE - requested_size
bit       2   QUARANTINED          given up on: never reused, never merged
bit       1   PREV_IN_USE          may the preceding footer be read?
bit       0   IN_USE
```

### requested_size does not need 64 bits

The old header stored the caller's exact request in a `size_t`. It does not
need one. A block is split whenever the remainder reaches `MM_MIN_BLOCK`, so
the distance between a block's extent and the payload inside it is bounded by a
small constant — and only that distance, the **slack**, has to be stored:

```
requested_size = block_size - MM_HDR_SIZE - slack
```

The bound has two parts, and it is worth deriving rather than guessing, because
the honest answer is not the obvious one:

1. **When the block is carved.** Either the trailer plus up to 15 bytes of
   rounding, or — for a request too small to reach `MM_MIN_BLOCK` — whatever
   the floor leaves over a one-byte request. Whichever is larger.
2. **An unsplit remainder.** A leftover below `MM_MIN_BLOCK` is kept rather
   than carved off, and it is a multiple of the alignment, so it adds at most
   `MM_MIN_BLOCK - 16`.

| Profile | carved max | + unsplit | **max slack** |
|---|---:|---:|---:|
| `fast` | 23 | 16 | **39** |
| `hardened` | 31 | 32 | **63** |
| `paranoid` | 39 | 32 | **71** |

Six bits would hold 63. It would fit `hardened` **exactly**, with no headroom
at all, and it would silently truncate under `paranoid`, whose 24-byte trailer
pushes the carved maximum to 39. The field is therefore **seven bits**, and
`mm_layout.h` derives `MM_SLACK_MAX` from the profile's own constants and pins
it with

```c
_Static_assert(MM_SLACK_MAX <= MM_W_SLACK_MASK, ...);
```

so a future profile that breaks the bound fails to compile rather than
returning wrong sizes. `tests/test_layout.c` then checks the round trip for
every request from 1 to 4096, and separately manufactures the unsplit-remainder
case across a sweep of request sizes and remainders, asserting that the largest
slack actually produced is exactly `MM_SLACK_MAX` — a bound the code cannot
reach would mean the derivation had drifted from the allocator.

`mm_requested_size` clamps rather than trusts the subtraction. Under a profile
with a checksum the slack has been vouched for by the time it is read; under
`fast` it has not, and an underflow there would produce a payload extent
reaching past the arena.

## CRC32C, and why the mirror could go

FNV-1a was replaced with **CRC32C**, dispatched at runtime to the SSE4.2
`crc32` instruction where the CPU has it and to a table-driven implementation
where it does not.

A 32-bit CRC over a 32-byte input detects **every 1-, 2- and 3-bit error**, and
misses a random error with probability 2⁻³². That guarantee is the justification
for this entire change: the mirrored footer was never adding *detection*, it
was adding *repair*. Dropping it from allocated blocks costs no detection at
all, which is why `hardened` sheds 48 bytes per allocation without shedding a
single detection guarantee, and why only `paranoid` keeps a mirror.

`tests/test_crc32.c` asserts the standard `CRC32C("123456789") == 0xE3069283`
check vector, flips every bit of a 12-byte buffer and requires the checksum to
change each time, and — most importantly — runs a few thousand random buffers
through both the hardware and software paths and requires them to agree.
Without that last test a fault-injection result would depend on which machine
produced it, and two runs could not be compared.

## Block confusion

The checksum covers the control word, the payload checksum, **the block's index
within the arena**, and **a per-arena secret**:

```c
hdr_crc = CRC32C(word, payload_crc, index, secret)
canary  = MM_CANARY ^ secret ^ (index * 0x9E3779B97F4A7C15)
```

Mixing in the index and the secret turns both from bit-flip detectors into
*block-confusion* detectors. A header copied verbatim from elsewhere in the
arena — internally beyond reproach, correct in every field — fails, because it
was checksummed for a different position. `tests/test_layout.c` copies one
block's header over another of identical size and requires it to be rejected,
and does the same with the canary.

The secret is drawn once per arena in `mm_init` from a stack address and the
clock. It is a corruption detector, not a security boundary, and is not claimed
to be one: it makes accidental confusion detectable, not deliberate forgery
impossible. `mm_pin_secret` fixes it so that the fault injector can replay a
trial byte for byte.

Including `payload_crc` under the header checksum is a small departure from the
obvious design and a deliberate one. It means a flip in the payload checksum is
reported as *header* damage rather than as payload damage — which is the useful
way round, because a healthy payload is then not surrendered on account of the
number describing it getting hit, and under `paranoid` the mirror can put it
back.

## Free blocks

```
[header] [next_free ^ secret] [prev_free ^ secret] ...unused... [footer = block_size]
```

The free-list links live in payload space the block is not using, so they cost
nothing. XOR-ing them with the arena secret means a stray write that happens to
look like a pointer does not survive validation.

There is no single free list. Blocks are filed by size into 128 bins — 64
exact classes covering everything up to about a kilobyte, then 64 log-spaced
bins at four per octave — with a 128-bit bitmap saying which are non-empty, so
the smallest sufficient bin is found by a mask and a `ctz` rather than by
looking. `src/mm_freelist.h` sets out how the sizes are cut and why the index
being monotonic is the property everything else rests on.

The links are the most attacked structure in any allocator and no checksum
covers them, so unlinking validates in a fixed order before it dereferences
anything: in the arena and aligned, then the header checksum, then free and in
*this* bin, then the two back-links. Only after all four does a splice happen.

The last eight bytes are the **boundary tag**: a free block repeats its own
extent there so the block after it can step backwards in O(1). This is legible
**only** when the following block's `PREV_IN_USE` is clear. Read at any other
time those bytes are payload, or the tail of a mirror — this is the sharpest
edge in the whole layout, and `mm_prev_free_block` is the only place allowed to
make the step. It refuses unless the tag, the header it claims to describe, and
that header's own extent all agree.

Neither the links nor the tag are covered by any checksum, because both live in
storage that belongs to the payload. They are defended structurally instead:
every traversal validates the node it lands on and the back-link that should
point at it, and anything that disagrees causes the free list to be **rebuilt
from the tiling** and reported as `MM_ERR_CORRUPT_LINKS`. The tiling is the
authority; the free list is a cache over it.

## Walking the heap

`mm_check_heap` steps `block += block_size` from `lo` to `hi` — the implicit
list — and only afterwards holds the free list up against what it found. That
is the right way round: geometry cannot quietly disagree with itself, whereas
two sets of pointers can. It checks that

- every header stands up, and every canary,
- `PREV_IN_USE` agrees with what actually precedes each block,
- every free block's boundary tag repeats its extent,
- no two free blocks are adjacent (a missed coalesce),
- the blocks tile `[lo, hi)` **exactly**,
- the quarantined blocks account for exactly `lost_bytes`, and
- the bins contain precisely the free blocks the walk found — every one in
  exactly one bin, in the bin its size asks for, no allocated block in any of
  them, back-links all agreeing, and a bitmap that says the same thing about
  which bins are empty.

## Covering what nobody touches

O(1) allocation means the allocator stops touching most of the arena. The
linear search it replaced revalidated every free block on every call, so damage
in cold memory was found incidentally; binning takes that away, and ignoring
the loss would trade the project's whole reliability story for throughput.

Two tiers replace it. **Validate on touch** is unchanged and O(1): every block
popped by malloc, every block freed, both coalescing neighbours. **`mm_scrub`**
is a bounded patrol over the implicit list, resuming where it stopped, run
automatically every `N` allocator calls — 1024 by default, sixteen blocks at a
time, both settable through `mm_set_scrub_interval`. It is what a hardware ECC
scrubber does, and the resemblance is the argument for it rather than a
decoration.

What the patrol does with what it finds is what every other path does: a broken
canary quarantines the block, an unreadable header goes through recovery, and a
free block's boundary tag is rewritten from the extent its checksum has already
vouched for — the one repair available under every profile. A payload that no
longer matches its checksum is *reported and left alone*, unlike on the read
path, because the patrol was not asked for those bytes and writing through the
returned pointer is permitted; destroying a live block on the strength of a
checksum the caller never promised to maintain would be the wrong trade.

The cost is a curve rather than a constant, and `bench/results/scrub-sweep.csv`
measures it: mean calls between a flip and the allocator noticing, against the
scrub interval. Damage to a free header or a link is found in ten to fifteen
calls whatever the interval, because those sit on the allocation path. Damage
to an allocated block's header, canary or payload tracks the interval directly
— and with the patrol off is never found by traffic at all.

Quarantine no longer removes a block from anything. A block given up on stays
in the tiling as permanently-allocated space nobody owns, flagged
`QUARANTINED`. That is what lets the tiling check be exact rather than
approximate: surrendered space is *countable*, not merely missing.

## Recovering from a damaged header

A header that fails its checksum has lost the one thing needed to step past it:
its extent. The walk resynchronises by scanning forward for the next header
that stands up on its own and tiles with what follows it. A real block is at
least `MM_MIN_BLOCK` long, so the scan starts there.

Under `paranoid`, that scan is what makes the mirror usable. A mirror sits at a
fixed offset from its block's **end**, and the end is exactly what the
resynchronisation point is — so once the scan has a foothold the repair is a
single bounded check rather than the search over every candidate extent that
the previous layout needed. The mirror carries its own index-bound checksum, so
a mirror belonging to any other block is refused.

Under `hardened` and `fast` there is no mirror, and the damaged span is
surrendered as one accounted quarantine block. Either way the cost is bounded
by the damaged block, not by everything downstream of it.

This is the one place the brief's shorthand of "O(1) repair from the mirror"
needs qualifying. The repair itself is O(1); *finding where the block ends* is
not, because that is precisely the information the damage destroyed. The scan
is bounded at `MM_RECOVERY_STEPS` alignment units.

## What each profile detects

Detection is what the metadata buys, so a profile carrying less metadata
detects less. Stating that plainly is worth more than a table of ticks.

| | `fast` | `hardened` | `paranoid` |
|---|:-:|:-:|:-:|
| Header bit flip | no | detected | detected **and repaired** |
| Payload bit flip | no | detected | detected |
| Overrun past the payload | no | detected | detected |
| Damaged free-list link | detected | detected | detected |
| Damaged boundary tag | detected | detected | detected |
| Block confusion | no | detected | detected |
| Never leaves the arena | yes | yes | yes |

`fast` has no checksum and no canary; there is nothing for it to detect a
flipped header bit *with*, and `tests/test_integrity.c` says so explicitly
rather than leaving a silent gap where the detection tests would be. What it is
*meant* to keep is the structural promise every profile makes: bounded
traversals, clamped extents, and no access outside the arena whatever the
control word says. That promise is gated in CI for all three profiles — a crash
in any fault-injection cell fails the build.

**Under `fast` that promise does not currently hold.** Two trials in 240,000
write about 115 MB past a 262 KB arena, both reproducible from a seed; the
mechanism and the reproducers are in
[FAULT_MODEL.md](FAULT_MODEL.md#where-that-promise-currently-does-not-hold).
Losing detection is the trade `fast` is supposed to be making. Losing the
bound is not, and the two should not be confused.

The single-bit header cell is decidable from theory for the two profiles with a
checksum, so CI gates it at 100% detection and zero silent corruption. `fast`
is measured on the same sweep and its numbers are recorded rather than gated;
leaving it out would hide the cost of the trade instead of pricing it.
