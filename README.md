# Mars Memory Allocator

A fault-tolerant memory allocator in C11. It allocates in O(1) from size-classed
free lists indexed by a bitmap, carries integrity metadata on every block so a
bit flipped underneath it is detected rather than silently used, gives each
thread its own arena, and ships an `LD_PRELOAD` shim that runs real programs.

The fault it models is the single-event upset: ionising radiation flips a bit in
DRAM and a value changes with nothing having written it. That is reproducible
exactly by XOR-ing a bit, which is what the fault injector does 1,200,000 times,
checking every payload against a shadow model of what it should contain.

## Results

7 targets x {1, 2, 4, 8} flipped bits x 10,000 trials per cell, each trial
forked under an alarm and classified against the shadow model.

| Profile | Metadata / allocation | Trials | Coverage | Silent | Crashes |
|---|---:|---:|---:|---:|---:|
| `fast` | 8 B | 240,000 | 74.13% | 41,022 | 0 |
| `hardened` (default) | 24 B | 280,000 | 100.00% | 0 | 0 |
| `paranoid` | 40 B | 280,000 | 100.00% | 0 | 0 |

Coverage is `detected / (detected + silent)`. Flips landing in slack, padding or
free space are excluded from the denominator, since there was nothing there to
catch. The zero-crash column is the promise every profile makes
unconditionally: no corrupted control word causes a read or write outside the
arena, whether or not the corruption is detectable.

Throughput against glibc `malloc` through the same workload code, median of 11
pinned repetitions, 400,000 operations each, 64 MB arena:

| Workload | `hardened` ops/s | glibc ops/s | ratio | p50 | util% |
|---|---:|---:|---:|---:|---:|
| `seq_lifo` | 5,031,258 | 32,293,228 | 0.16x | 126 ns | 66.7 |
| `seq_fifo` | 4,994,059 | 33,372,869 | 0.15x | 131 ns | 66.7 |
| `random_size` | 4,458,736 | 18,625,024 | 0.24x | 133 ns | 97.4 |
| `churn` | 5,301,577 | 22,724,488 | 0.23x | 114 ns | 99.4 |
| `realloc_grow` | 3,788,084 | 3,773,644 | 1.00x | 181 ns | 100.0 |
| `fragmentation` | 2,818,612 | 15,754,142 | 0.18x | 142 ns | 94.3 |
| `validated_access` | 373,669 | 2,356,452 | 0.16x | 144 ns | 100.0 |

**Four to seven times slower than glibc per operation.** glibc keeps a
per-thread cache in front of its free lists and does no integrity checking at
all. `realloc_grow` is where in-place growth into an adjacent free block keeps
up. `validated_access` pays CRC32C over the whole payload on every read, which
is the cost of the feature, not of the allocator.

### Threads

`mt_churn` is T independent copies of `churn` and shares nothing.
`producer_consumer` has half the threads allocating and half freeing, so every
block is released by a thread that did not allocate it.

| Threads | `mt_churn` global lock | | per-thread arenas | |
|---:|---:|---:|---:|---:|
| 1 | 5.82M ops/s | 1.00x | 5.17M ops/s | 1.00x |
| 2 | 3.08M ops/s | 0.53x | 9.02M ops/s | 1.74x |
| 4 | 1.79M ops/s | 0.31x | 12.04M ops/s | 2.33x |
| 8 | 0.84M ops/s | 0.15x | 18.00M ops/s | 3.48x |

| Threads | `producer_consumer` global lock | | per-thread arenas | |
|---:|---:|---:|---:|---:|
| 1 | 4.04M ops/s | 1.00x | 4.09M ops/s | 1.00x |
| 2 | 2.14M ops/s | 0.53x | 4.86M ops/s | 1.19x |
| 4 | 1.42M ops/s | 0.35x | 8.48M ops/s | 2.07x |
| 8 | 0.60M ops/s | 0.15x | 9.66M ops/s | 2.36x |

A single global mutex scales negatively: eight threads complete less total work
than one. Contention is half of it; the rest is false sharing, since every
thread touches the same bins, bitmap and counters, so each acquisition also pays
for cache lines the previous holder invalidated. glibc reaches 4.00x on
`mt_churn` through the same harness, which puts the flat curve down to the
allocator, not on the benchmark. Both strategies stay selectable at build time
and both are tested.

### Under real programs

Whole-process wall time under `LD_PRELOAD`, median of 11 repetitions, allocators
alternating repetition by repetition:

| Program | glibc | `hardened` | ratio | peak RSS |
|---|---:|---:|---:|---:|
| `ls_recursive` | 25 ms | 35 ms | 1.40x | 12 MB |
| `git_status` | 218 ms | 202 ms | 0.93x | 12 MB |
| `git_log_stat` | 4,331 ms | 3,932 ms | 0.91x | 12 MB |
| `grep_recursive` | 46 ms | 50 ms | 1.08x | 12 MB |
| `python_sum` | 24 ms | 30 ms | 1.24x | 12 MB |
| `python_dict` | 127 ms | 105 ms | 0.83x | 47 MB |
| `gcc_compile` | 209 ms | 246 ms | 1.18x | 26 MB |

A ratio near 1.0 means the allocator disappeared into everything else the
process was doing, not that it matched glibc per operation. Every program
produced byte-identical output, with `mm_check_heap()` clean after each run.

| `calloc` shape | glibc | `hardened` | zeroing shortcut off |
|---|---:|---:|---:|
| `calloc_4mb_x200` | 31 ms | 11 ms | 293 ms |
| `calloc_4kb_x200000` | 13 ms | 72 ms | 72 ms |

A large `calloc` is served from a mapping the kernel has just supplied, whose
pages are already zero, so the `memset` is skipped entirely; the third column is
the same build forced to perform it. A small one comes from recycled arena
memory where the `memset` is unavoidable and glibc's per-size cache wins.

Every number above is generated from a committed CSV in
[bench/results/](bench/results/), and `tools/check_readme_numbers.py` fails CI
on any numeric claim that no CSV, `#define` or declared definition backs.
[docs/RESULTS.md](docs/RESULTS.md) carries the full tables.

## Block layout

```
                      arena, [lo, hi)
  +--------+--------------+--------+--------------------+--------+
  | block  | block        | block  | block              | block  |   the tiling
  +--------+--------------+--------+--------------------+--------+   is the list
     used       free         used          free            used
                  |                          |
                  +------------+-------------+
                               v
                     128 size-classed bins + a 128-bit bitmap
                     0..63   exact classes, MM_MIN_BLOCK + 16i
                     64..127 log-spaced, 4 sub-bins per octave
                     a cache over the tiling, never the authority
```

Blocks tile the arena exactly, so stepping forward is `b + block_size` and there
is no adjacency list that can fall out of step with the memory it describes. An
allocated block under the default profile costs 24 bytes:

```
  +0    control word (8 B)     block_size | slack | QUARANTINED | PREV_IN_USE | IN_USE
  +8    hdr_crc | payload_crc  CRC32C over the word, the payload CRC,
                               the block's index and a per-arena secret
  +16   payload ...............................................
  +..   canary (8 B)           bound to the same index and secret
```

**The control word packs the extent, the caller's exact request and three flags
into 64 bits.** Block sizes are 16-granular, so the extent needs 54 bits after
shifting. `requested_size` is not stored at all: splitting guarantees the
distance between a block's extent and the payload inside it is bounded by a
small constant, so only that *slack* is kept, in 7 bits, and the request is
recovered as `block_size - MM_HDR_SIZE - slack`. The bound is derived from each
profile's own constants and pinned by a `_Static_assert`, with a test allocating
every size from 1 to 4096 and asserting the round trip is exact. The bound is
tight, not merely sufficient: the worst slack a test can produce equals it, and
the widest profile needs a value a 6-bit field could not have held.

**Free blocks pay for their own bookkeeping.** A free block spends its unused
payload on `next ^ secret`, `prev ^ secret` and a boundary tag repeating its
extent, so backward coalescing is O(1) and an allocated block carries no footer
at all. A `PREV_IN_USE` bit in the following header says whether reading
backwards is legal; read at any other time those bytes are payload. XOR-ing the
links with a per-arena secret means a stray write that happens to look like a
pointer does not survive validation.

Three profiles select the layout at compile time:

| | `fast` | `hardened` (default) | `paranoid` |
|---|---:|---:|---:|
| header | 8 | 16 | 16 |
| canary / tail mirror | none | 8 / none | 8 / 16 |
| metadata per allocation | 8 | 24 | 40 |
| detects a header bit flip | no | yes | yes, and repairs it |

## O(1) allocation

Bins 0 to 63 are exact classes holding blocks of exactly `MM_MIN_BLOCK + 16i`,
covering every request up to about a kilobyte, so the head of the bin is the
answer and there is no search. Bins 64 to 127 are log-spaced at four sub-bins
per octave, where blocks differ in size and a first-fit scan runs under a cap of
16, giving a good fit instead of a best fit.

The search is O(1) because `mm_bin_of` is monotonic non-decreasing in block
size. Every bin above the one a request maps to therefore holds only strictly
larger blocks, so its head fits without being examined, and only the request's
own bin is ever scanned. A `uint64_t[2]` bitmap finds the smallest non-empty bin
above it with a mask and `__builtin_ctzll`, so runs of empty bins cost nothing.
Monotonicity is checked across every 16-granular size the arena can hold,
including the join between the two schemes and the clamp on the top bin.

Unlinking validates before dereferencing anything, in the order glibc's hardened
unlink uses: inside the arena and aligned, then the checksum, then free and
mapped to *this* bin, and only then the back-links. The links live in payload
space and no checksum covers them, so a node whose neighbours do not corroborate
it is never spliced through. Anything finding the bins inconsistent rebuilds
from the tiling, which is the authority, and reports `MM_ERR_CORRUPT_LINKS`.

## Detecting corruption

`hardened` and `paranoid` checksum the header with **CRC32C, dispatched at run
time to SSE4.2 `_mm_crc32_u64` where the CPU has it and a table-driven fallback
where it does not**, with a unit test asserting both paths agree over thousands
of random buffers. A 32-bit CRC over a 32-byte header detects every 1-, 2- and
3-bit error outright and misses a random one with probability 2^-32, which is
why dropping the mirrored footer cost no detection. A second copy buys *repair*,
not detection, and that is the only thing `paranoid` spends its extra 16 bytes
on.

**Mixing the block's index and a per-arena secret into both the checksum and the
canary turns them from bit-flip detectors into block-confusion detectors.** A
header copied verbatim from elsewhere in the arena, correct in every field,
fails validation because it was checksummed for a different position. The same
binding is what makes repair trustworthy: under `paranoid` a damaged header is
rebuilt from its tail mirror only if that mirror's checksum holds *for the index
of the position it would restore*, so a mirror belonging to another block is
refused. A wrong repair is worse than none, since it would hand back a block of
the wrong extent overlapping a live neighbour.

Where a walk writes into a block on the strength of having found it, a
bounds-plausible header is not enough. `mm_extent_corroborated` requires a free
block's boundary tag to agree with its control word, which is what separates a
block from a run of payload bytes that happens to read like one under a profile
with no checksum. Any extent read before a bin operation is re-established
afterwards, because a bin operation writes into free blocks and a rebuild
triggered inside one writes into every free block it reaches, so a size that
crossed such a call is not a block size until it has been asked again.

Blocks the allocator gives up on stay in the tiling as permanently-allocated
space marked `QUARANTINED`, never merged and never handed out. That turns lost
space from an inference into an arithmetic identity `mm_check_heap` verifies:
the tiling must cover the arena exactly, and surrendered bytes must equal the
running total.

## The scrubber

O(1) allocation means the allocator stops touching most of the arena, so
corruption in cold memory would sit unnoticed. A linear best-fit search
revalidates every free block on every call: expensive, but incidentally complete
continuous coverage. Bins remove both together, so `mm_scrub` replaces the
coverage with a bounded patrol over the implicit list, resuming where it stopped
and invoked every 1024 allocator calls by default.

What that costs is a curve, not a constant, and it is measured. Damage to
a free header or a link sits on the allocation path and is found in ten to
fifteen calls whatever the patrol is set to; damage to an allocated header,
canary or payload is the patrol's alone, and with the patrol off is never found
by ordinary traffic. The scrub interval is a mandatory column in every
fault-injection CSV, since results at different settings are otherwise not
comparable.

## Threads

Each thread allocates from its own arena, named by a thread-local pointer
declared `initial-exec` so reading it is one load off `%fs` rather than a call
into `__tls_get_addr`. Every function is still written against `g_arena`, a
macro that resolves to the calling thread's arena under `MARS_LOCK=arena` and to
the single static arena otherwise, which kept the change out of the allocation
path entirely.

**Ownership is resolved through a span registry, not from the address itself.**
The allocator maps 2 MB-aligned chunks, so shifting a pointer right by 21 names
the mapping it falls in while spending zero header bits. The obvious next step,
masking the pointer down to that boundary and dereferencing it, is a read of
memory chosen by the caller: safe for a pointer this allocator handed out and
unsafe for every other kind. A libc shim is handed foreign pointers routinely,
and the aligned address below one need not be mapped at all, so that read
segfaults inside `free`. The shift instead keys an open-addressed table of spans
the allocator mapped itself. A miss is a foreign pointer and costs one probe,
and only a hit is dereferenced.

Readers of that table take no lock, because a reader-writer lock in every `free`
would reintroduce the serialisation per-thread arenas exist to remove. Writers
serialise on a mutex of their own, and three properties make lock-free reads
safe: growing publishes a new table and leaves the old one mapped, so a reader
holding the old pointer reads memory that is still there; deletion leaves a
tombstone instead of shifting entries back past a concurrent probe; and span
descriptors come from a pool that is never returned, so a reader arriving just
after a span is released finds a descriptor that is still there and disowned.

A free of a block owned by another thread is pushed onto the owner's remote-free
stack with an atomic exchange and drained by the owner in one take, which avoids
ABA without tagged pointers and is what stops `producer_consumer` serialising.

**In-band checksummed metadata forces one unusual rule.** A header read while
another thread rewrites a neighbour's `PREV_IN_USE` does not produce a torn
size; it produces a control word and a CRC that disagree, which this allocator
would report as corruption that never happened. A fault-tolerant allocator
cannot cry wolf, so block metadata is only ever touched under its own arena's
lock, read-only entry points included: `mm_verify` and `mm_check_heap` are
exactly the calls a racing header would lie to.

## Validated access, and what a raw pointer costs

An allocator that returns a pointer has at that instant given up the ability to
say anything about the bytes behind it: the caller can write through it without
telling anyone, and any payload checksum is stale from the next store. A
checksum that is *sometimes* stale is worse than none, because it produces false
reports. So payload integrity is offered only where it can be maintained.
`mm_write` refreshes the payload CRC after copying in, `mm_read` verifies it
before copying out, and `mm_malloc` still returns an ordinary pointer. The
header, canary and boundary tags are protected either way, because the allocator
is their only writer.

The shim cannot require that API, so the trade is named explicitly:

| | `MM_MODE_MANAGED` (default) | `MM_MODE_LIBC` |
|---|---|---|
| Access | `mm_read` / `mm_write` | raw pointers |
| Header CRC, canary, boundary tags, free lists | yes | yes |
| Payload checksum | maintained and checked | not maintained |
| `mm_verify` on an intact block | `MM_OK` | `MM_ERR_DEGRADED` |

`MM_ERR_DEGRADED` does not mean "probably fine". It means everything this mode
can check was checked and was sound, and the payload was not looked at.
Returning `MM_OK` would claim a guarantee that had stopped being provided.
Damage that is still detectable outranks it, so a broken canary reports
`MM_ERR_CORRUPT_CANARY` in either mode.

Two consequences follow. Switching into `MM_MODE_LIBC` clears every payload
checksum already established, since leaving them would report the first
legitimate store through a raw pointer as corruption. And a libc-mode block
records the whole of itself less its trailer as payload, because
`malloc_usable_size` must return a count the program may write every byte of;
ending the payload at the requested size would put the canary inside that
entitlement and flag a correct program as overrunning.

## The preload shim

`libmars_preload.so` exports `malloc`, `calloc`, `realloc`, `free`,
`aligned_alloc`, `posix_memalign`, `memalign`, `valloc` and
`malloc_usable_size`, plus glibc's internal `__libc_*` aliases, which the C++
runtime and some libraries call directly and which would otherwise split a
program's allocations across two allocators.

Three problems dominate the implementation:

- **Bootstrap recursion.** `dlsym(RTLD_NEXT, "free")` itself allocates, through
  `calloc`. Early allocations are served from a static pool behind a flag, and
  freeing a pool pointer is a no-op.
- **Foreign pointers.** A program may free memory allocated before the shim was
  loaded. The span registry answers ownership in one probe without dereferencing
  anything, and misses route to the real `free`.
- **Diagnostics inside `malloc`.** Every `printf`-family function allocates, so
  the shim writes with `write(2)` and nothing else.

`aligned_alloc` over-allocates, places the header immediately below the returned
pointer and records the shift in a flag so `free` recovers the true block start.

## Verification

| | |
|---|---|
| Unit and property tests | 161 cases over 12 binaries, under gcc and clang, Debug and Release, three metadata profiles and three locking strategies |
| Differential fuzzer | every operation checked against a shadow model, seeded so any failure replays exactly |
| Sanitizers | ASan and UBSan under gcc and clang, ThreadSanitizer in a job of its own, Valgrind memcheck over every test binary and a long fuzz run |
| Threads | concurrent allocation, cross-thread frees in both directions, a full remote-free queue, a resize of another thread's block, and a thread exiting with live blocks, all under TSan and again under the global lock |
| Fault injection | 56,000 trials per profile per push, gated on the arena promise under all three profiles and on the single-bit header cell |
| Coverage | gated at 90% lines on `src/`, not merely reported |
| Portability | Windows UCRT64 builds the core, tests, fuzzer and benchmark |

**The ThreadSanitizer job carries its own negative control.** A green sanitizer
run says nothing unless the same run would have failed on code that is actually
racy, so the job builds the same tests once more with `MARS_LOCK=none` and fails
if ThreadSanitizer does *not* report a race there.

The single-bit header cell is gated because theory already knows the answer: a
32-bit CRC detects every single-bit change in its input without exception, so
under any profile carrying one that cell must come out at 100% detection with
zero silent corruption. Anything else is a defect, not a statistic.

Every branch guarding an extent across a bin operation was verified by deleting
it and confirming that exactly one test, its own, fails. Where a branch is
unreachable under a profile the test is not compiled at all, and the layout
reason it is unreachable is recorded.

## What this is not

- **Not radiation hardening.** Real hardening is ECC memory, redundant hardware
  and physical shielding. Nothing in software stops a bit flipping; this notices
  afterwards.
- **Not lock-free.** Every entry point touching block metadata runs inside its
  arena's lock, read-only ones included, for the false-positive reason above.
  That lock is uncontended for the owning thread, and what it costs a
  single-threaded program is measured, not assumed.
- **Not a per-thread cache in front of the free lists.** glibc has one and this
  does not, which is much of the absolute gap. After per-thread arenas there is
  no contention left for such a cache to remove, and a cached block is one the
  tiling calls allocated and nobody owns, which every accounting invariant would
  need an exception for.
- **Not a complete `malloc` replacement.** The shim is Unix-only and has no
  equivalent of glibc's per-size caching, which the small-`calloc` row shows
  costing an order of magnitude.
- **Not able to repair a payload.** Repair exists only for metadata, only under
  `paranoid`, and only while the mirror survives. A flip in user data is
  reported and the data is gone.
- **Not benchmarked on a quiet machine.** Taken on a laptop under WSL2, where
  repeated runs move glibc's own figures substantially. One significant figure
  is what the throughput numbers support; the fault-injection counts are seeded
  and deterministic and do not have this problem.

## Build

Ubuntu 22.04, gcc 11.4, CMake 3.22.

```bash
cmake --preset gcc-release
cmake --build --preset gcc-release -- -j$(nproc)
ctest --preset gcc-release --output-on-failure
```

Presets select the profile (`gcc-fast`, `gcc-paranoid`), the locking strategy
(`gcc-nolock`, `gcc-lock-global`), and the sanitizers.
[bench/results/README.md](bench/results/README.md) lists every committed CSV
with the command that produced it, and
[docs/BENCHMARK_METHOD.md](docs/BENCHMARK_METHOD.md) sets out the timing
protocol: separate clocks for throughput and latency, timer overhead measured
and subtracted and disclosed, a log-linear histogram with bounded relative
error, one discarded warmup, and medians over repetitions written out
individually.

Design detail is in [docs/DESIGN.md](docs/DESIGN.md),
[docs/BLOCK_LAYOUT.md](docs/BLOCK_LAYOUT.md) and
[docs/FAULT_MODEL.md](docs/FAULT_MODEL.md).

## Licence

MIT. See [LICENSE](LICENSE).
