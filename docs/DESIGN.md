# Design

The mechanisms are in the [README](../README.md): block layout, the bins, the
checksum, the scrubber, per-thread arenas and the shim. This file carries the
decisions behind them and the properties everything is written against.
[BLOCK_LAYOUT.md](BLOCK_LAYOUT.md) is the authority on the shape of a block,
[FAULT_MODEL.md](FAULT_MODEL.md) on what corruption is defended against, and
[BENCHMARK_METHOD.md](BENCHMARK_METHOD.md) on how the numbers were taken.

## Validated access, and the two modes

An allocator that hands back a raw pointer has at that instant given up the
ability to say anything about the bytes behind it. A checksum that is
*sometimes* stale is worse than none: it produces false reports, and a detector
nobody believes is not a detector. So payload integrity is offered only through
`mm_read` and `mm_write`, and the trade is a value the caller sets:

| | `MM_MODE_MANAGED` (default) | `MM_MODE_LIBC` |
|---|---|---|
| Access | `mm_read` / `mm_write` | raw pointers |
| Header checksum, canary, boundary tags, free-list validation | yes | yes |
| Payload checksum | maintained and checked | **not maintained at all** |
| `mm_verify` on an intact block | `MM_OK` | `MM_ERR_DEGRADED` |

Three details of that are easy to get wrong, and all three are in the code
rather than only in this paragraph.

**The patrol and the read path treat a payload mismatch differently.** A block
written through its raw pointer keeps whatever payload checksum it last had.
`mm_read` treats a mismatch as corruption, because the caller asked for those
bytes and must not be handed wrong ones. The patrol only reports it and leaves
the block alone: it was not asked for anything, and destroying a live block over
a checksum the caller never promised to maintain would be the wrong trade.

**`payload_crc == 0` means not established, and `mm_malloc` leaves it that
way.** The caller has not written anything yet, and establishing it eagerly
would make every allocation O(size). Switching into `MM_MODE_LIBC` clears every
checksum already established, since leaving them would report the first
legitimate store through a raw pointer as corruption. Switching back does not
invent them again.

**`MM_ERR_DEGRADED` is not returned under `fast`.** That profile carries no
payload checksum in either mode, so there is no mode-imposed limitation to
report. `mm_verify` returns `MM_ERR_DEGRADED` when the *mode* prevented a check;
conflating a compile-time absence with a run-time mode would make one status
mean two things. What a profile carries is answered by `mm_profile()` and
`mm_metadata_overhead()`.

What the mode does not change is the point of having it: the layout, the
profile, the header checksum, the canary, the boundary tags, the free-list
validation, quarantine, recovery and the patrol are identical. `MM_MODE_LIBC`
gives up one of four kinds of integrity checking and keeps three. Under
`hardened` that is 24 bytes per allocation still covering every byte of metadata
a bit flip could land in, which `MARS_SHIM_FLIP` stages inside real programs.

## Architecture

```
include/mars/allocator.h   the public surface: 18 functions, mm_status_t, mm_stats_t
        |
src/mm_layout.h            block shape, control word, accessors, per profile
src/mm_freelist.{h,c}      bin indexing, the 128-bit bitmap, hardened unlink
src/mm_internal.h          arena state, spans, constants, internal declarations
src/mm_arena.{h,c}         where memory comes from: providers, chunk growth,
                           the span registry, MADV_DONTNEED
src/mm_lock.{h,c}          the locking strategy, per-thread arenas, and the
                           queue a free uses when it crosses between them
src/mm_core.c              init, malloc, free, realloc, memalign, split, coalesce
src/mm_integrity.c         checksums, canary, validation, recovery, quarantine,
                           modes, mm_check_heap, mm_scrub
src/mm_access.c            mm_read / mm_write
src/mm_crc32.{h,c}         CRC32C: runtime SSE4.2 dispatch, table fallback
src/mm_stats.c             per-arena counters, compiled out unless MM_STATS

shim/mm_shim.c             the LD_PRELOAD malloc family, Unix only
```

Everything that knows how a block is shaped lives in `mm_layout.h`, and the rest
is written against its accessors. That is what makes the integrity profile a
compile-time choice instead of a fork of the allocator.

A build is configured along three axes, orthogonal to each other and to
everything above: `MARS_PROFILE` selects how much integrity metadata a block
carries, `MARS_STATS` whether the counters exist, and `MARS_LOCK` which of the
three locking strategies is compiled in. Each is compile-time because each
changes a structure layout, and a program linking against a library built with a
different answer would read it at the wrong offsets.

There are two ways to install an arena, and they are alternatives, not layers.
`mm_init` takes a caller-supplied buffer, zeroes it once so a payload checksum
never covers indeterminate memory, and cannot grow it. `mm_arena_init_growable`
maps memory itself and maps more when it runs out, which is what the shim uses,
because a program under `LD_PRELOAD` cannot be asked how much heap it will need.
Either way a per-arena secret is drawn, binding every checksum and canary to the
arena it was computed in.

## Growth, and why a block belongs to a span

A fixed buffer is one contiguous tiling. Growth cannot be: a caller's buffer
cannot be extended, and neither can a mapping with something already mapped
after it, so more memory means another region, and two regions are not one
tiling. Everything reasoning about a block's geometry therefore asks which
**span** the block is in first, and answers against that span's bounds instead
of a single global pair.

Blocks never straddle a span, which is what keeps the rest untouched: inside a
span, coalescing, the boundary tags, the backward walk and the consistency check
are exactly what they were. The first block of every span reports its
predecessor as in use, which stops a backward walk stepping out of the mapping.
The bins are shared across spans, because a free block is filed by its size and
nothing else, and that is what keeps allocation O(1) however many chunks exist.

Chunks are 2 MB and 2 MB-aligned, obtained by over-mapping one extra chunk and
trimming both ends. The alignment buys one thing: the span a pointer belongs to
is identified by a shift, spending zero header bits. There are none to spend,
since every field in the control word is load-bearing and the whole word is
checksummed.

An allocation larger than half a chunk gets a mapping of its own, before the
bins are consulted at all. That is not a performance choice. It is what makes
releasing it hand memory back to the operating system rather than only to the
free lists, because a multi-megabyte buffer that happened to fit inside an
existing chunk would pin that chunk for the life of the process. Ordinary chunks
are kept and reused, since churning mappings for ordinary allocations would
trade a free-list operation for two syscalls and a storm of page faults. What
they hand back instead is the resident pages of a large enough free run, through
`MADV_DONTNEED`.

## Threads

The scaling curves and the span-registry design are in the README. Three things
about the boundaries of that design belong here.

**A caller-supplied arena cannot be divided.** It is one fixed region, so there
is nothing to give a second thread. Under `MARS_LOCK=arena` a program that
called `mm_init` gets exactly one arena and every thread contends for its lock,
which is `MARS_LOCK=global`'s behaviour arrived at honestly instead of a
per-thread design quietly not happening. `mm_arena_count()` stays at 1 and says
so, and the thread-scaling runs use a growable arena for that reason: measuring
per-thread scaling against a fixed buffer would measure the fallback. A growable
arena is also what a threaded program actually gets, since it is what the shim
installs.

**Arenas outlive their threads.** A thread that exits with blocks still live
does not take them with it. Its arena is not destroyed and its memory is not
reclaimed; the blocks stay allocated, readable, verifiable and freeable by
whoever holds them. The arena stops being spoken for and goes into a pool, so
the next thread that needs one takes it instead of making another, and a program
creating threads in a loop does not accumulate an arena per thread.

**No thread cache in front of the bins.** It is the usual next move, but its job
is to keep a thread off a lock it would otherwise contend, and after per-thread
arenas each thread's bins are already its own and its lock is already
uncontended. What is left for a cache to remove is a handful of pointer
operations, not contention, and it would cost something real: a cached block is
allocated as far as the tiling is concerned but owned by nobody, which is the
shape of a quarantined block, and it would have to be excluded from
`live_blocks`, from the utilisation figures and from what `mm_check_heap`
reconciles. That is a measurable amount of accounting to buy an unmeasured win.

One window per-arena locking does not close: a lookup for an address *inside a
mapping just released* can still be holding that span when the memory goes.
Reaching it requires a pointer into memory already freed, a use-after-free in
the caller and the same program error that makes glibc abort. Closing it would
mean deferring every unmap until every thread had been observed to leave the
allocator. `src/mm_arena.c` names it instead of implying it is impossible. The
shim's start-up and its fault-injection hook are likewise single-threaded, and
`shim/mm_shim.c` says which is which.

## Invariants

These are the properties everything else is written against. `mm_check_heap`
checks all of them, and the differential fuzzer runs it continuously.

- **The tiling is the list.** Blocks tile each span's `[lo, hi)` exactly,
  always. Stepping forward is `b + block_size`; stepping back over a free
  neighbour is its boundary tag. There is no adjacency list that can fall out of
  step with the memory it describes.
- **A block belongs to exactly one span, and never straddles two.** Every bound,
  every walk and every recovery scan stops at its span's edge, because what lies
  past that edge is a different mapping and not the allocator's to read or
  write.
- **The bins are a cache over the tiling and never the authority.** Anything
  finding them inconsistent rebuilds from the tiling and reports
  `MM_ERR_CORRUPT_LINKS`.
- **No two adjacent free blocks.** Two of them are a missed coalesce and
  `mm_check_heap` reports it. Coalescing takes both inputs out of their bins
  before their sizes change, and inserts the result into the bin its new size
  asks for.
- **Validate before dereferencing.** Unlinking checks in a fixed order: in the
  arena and aligned, then the header checksum, then free and in *this* bin, then
  both back-links, and only then splices.
- **Never write through an unvalidated block.** A block's control word decides
  where its own trailer lands, so sealing a block whose header failed would
  scatter writes across the arena.
- **Corroborate before writing into a block a walk *found*.** Stronger than the
  line above. `mm_header_ok` establishes that a block is what it says only where
  a checksum backs it; under `fast` it is a bounds check, and a run of payload
  bytes can pass it. So a walk that would write into a block because it found
  one there, meaning the bin rebuild filing free blocks or the resynchronisation
  scan deciding where an abandoned span ends, asks `mm_extent_corroborated`,
  which additionally requires a free block's boundary tag to repeat its extent.
- **A size that crossed a bin operation is not a size.** Bin operations write
  into free blocks, and one finding its list damaged rebuilds from the tiling
  and writes into more of them. An extent read before such a call is
  re-established after it, through `mm_unchanged`, before any arithmetic uses
  it.
- **Every surrendered byte is accounted for.** Space the allocator gives up is
  counted in `lost_bytes`, and a quarantined block stays in the tiling as
  permanently-allocated space nobody owns. That turns "space was lost" from an
  inference into an arithmetic identity the consistency check can verify.
- **`mm_last_error` is reset on entry, never on the way out.** A call that
  succeeds after stepping over corruption still reports what it saw, and the
  patrol never clears it.
- **A block's metadata is only touched under its own arena's lock.** Not just
  the writes, the reads too, because what a race produces here is a control word
  and a checksum that disagree, and that is indistinguishable from real
  corruption. `mm_last_error` is already `_Thread_local`, so what a thread saw
  stays that thread's.
- **At most one arena lock is held at a time.** That is the entire deadlock
  argument, and it is why a cross-arena `realloc` reads the old size under the
  owner's lock, gives it back, and only then allocates in its own arena.

## Threat model

The fault modelled is a single-event upset: ionising radiation flips one or more
bits in DRAM and a value changes with nothing having written it. Not an
attacker. The per-arena secret makes accidental block confusion detectable; it
is not a security boundary and is not claimed to be one.
[FAULT_MODEL.md](FAULT_MODEL.md) sets out the full taxonomy, what is checked
where, and how the numbers are produced.
