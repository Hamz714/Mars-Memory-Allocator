# Block layout

The shape of a block, the control word and the free-block interior are in the
[README](../README.md#block-layout). This file carries the two things too long
to put there: the derivation behind the slack field, and what each profile
actually detects.

| Profile | Header | Trailer | Metadata | + avg 16-align rounding | Avg total |
|---|---:|---:|---:|---:|---:|
| `fast` | 8 | 0 | **8** | 8 | 16 |
| `hardened` (default) | 16 | 8 | **24** | 8 | 32 |
| `paranoid` | 16 | 24 | **40** | 8 | 48 |

Selected at configure time with `-DMARS_PROFILE=`. The figures are checked, not
asserted: `tests/test_layout.c` allocates 64-byte blocks, reads `mm_stats_t`,
and requires `(peak_block_bytes - peak_payload_bytes) / peak_blocks` to come out
at 16, 32 and 48.

## Why the slack field is seven bits

`requested_size` is recovered as `block_size - MM_HDR_SIZE - slack`, so the
field only has to hold the distance between a block's extent and the payload
inside it. That distance is bounded because a block is split whenever the
remainder reaches `MM_MIN_BLOCK`. The bound has two parts, and the honest answer
is not the obvious one:

1. **When the block is carved.** Either the trailer plus up to 15 bytes of
   rounding, or, for a request too small to reach `MM_MIN_BLOCK`, whatever the
   floor leaves over a one-byte request. Whichever is larger.
2. **An unsplit remainder.** A leftover below `MM_MIN_BLOCK` is kept instead of
   being carved off, and it is a multiple of the alignment, so it adds at most
   `MM_MIN_BLOCK - 16`.

| Profile | carved max | + unsplit | **max slack** |
|---|---:|---:|---:|
| `fast` | 23 | 16 | **39** |
| `hardened` | 31 | 32 | **63** |
| `paranoid` | 39 | 32 | **71** |

Six bits would hold 63. It fits `hardened` exactly, with no headroom, and
silently truncates under `paranoid`, whose 24-byte trailer pushes the carved
maximum to 39. The field is therefore seven bits, and `mm_layout.h` derives
`MM_SLACK_MAX` from each profile's own constants and pins it:

```c
_Static_assert(MM_SLACK_MAX <= MM_W_SLACK_MASK, ...);
```

A future profile that breaks the bound fails to compile instead of returning
wrong sizes. `tests/test_layout.c` checks the round trip for every request from
1 to 4096, then manufactures the unsplit-remainder case across a sweep of
request sizes and remainders and asserts the largest slack actually produced is
exactly `MM_SLACK_MAX`. A bound the code cannot reach would mean the derivation
had drifted from the allocator.

`mm_requested_size` clamps instead of trusting the subtraction. Under a profile
with a checksum the slack has been vouched for by the time it is read; under
`fast` it has not, and an underflow there would produce a payload extent
reaching past the arena.

## What each profile detects

| | `fast` | `hardened` | `paranoid` |
|---|:-:|:-:|:-:|
| Header bit flip | no | detected | detected **and repaired** |
| Payload bit flip | no | detected | detected |
| Overrun past the payload | no | detected | detected |
| Damaged free-list link | detected | detected | detected |
| Damaged boundary tag | detected | detected **and repaired** | detected **and repaired** |
| Block confusion | no | detected | detected |
| Never leaves the arena | yes | yes | yes |

`fast` has no checksum and no canary, so there is nothing for it to detect a
flipped header bit with, and `tests/test_integrity.c` says so explicitly instead
of leaving a silent gap where the detection tests would be. What it does keep is
the structural promise every profile makes: bounded traversals, clamped extents,
and no access outside the arena whatever the control word says. CI gates that
promise for all three profiles, so a crash in any fault-injection cell fails the
build.

The boundary tag is the one row where `fast` detects but cannot repair, and the
asymmetry is worth following because it explains a rule in the allocator. A free
block spends eight of its unused payload bytes repeating its own extent. Under
`hardened` and `paranoid` the header checksum has already established that
extent, so the tag is a convenience and `mm_header_ok` is real verification.
Under `fast` `mm_header_ok` is a bounds check, and **the tag is the only second
copy of the extent that exists anywhere.**

Two consequences follow. A header and a tag that disagree under `fast` are two
unvouched copies of one number with nothing to say which is wrong, so the patrol
reports it and leaves both alone; rewriting the tag from the header would
manufacture the corroboration the rest of the allocator depends on. And any walk
that writes into a block *because it found one there*, meaning the bin rebuild
filing free blocks and the resynchronisation scan deciding where an abandoned
span ends, has to ask `mm_extent_corroborated` rather than `mm_header_ok`, since
a run of payload bytes can pass a bounds check. `mm_publish` positions both of
its writes from the extent, which is what makes that the difference between a
stray word and a stray write.

The single-bit header cell is decidable from theory for the two profiles with a
checksum, so CI gates it at 100% detection and zero silent corruption. `fast` is
measured on the same sweep and recorded rather than gated, because leaving it
out would hide the cost of the trade instead of pricing it.
[FAULT_MODEL.md](FAULT_MODEL.md) has the full taxonomy and the per-profile
reachability of every guard.
