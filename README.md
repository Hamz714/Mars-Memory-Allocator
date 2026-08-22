# Mars Memory Allocator

A memory allocator in C that assumes its memory is being corrupted underneath
it, and is built to notice. Every block carries integrity metadata — a
checksummed header, a canary past the payload, boundary tags on free space — so
that a bit flipped after the fact is detected rather than silently used. It
manages a caller-supplied arena, allocates in O(1) from size-classed free lists
over a bitmap, and patrols cold memory in the background.

The fault it models is the **single-event upset**: ionising radiation flips a
bit in DRAM and a value changes with nothing having written it. That is a real
failure mode for spacecraft and a convenient one to study, because it can be
reproduced exactly by XOR-ing a bit — which is what the fault injector does,
1,200,000 times, and then checks every payload against a shadow model of what
it was supposed to contain.

## Results

Under the default `hardened` profile, **280,000 fault-injection trials: 100%
detection coverage, 0 silent corruptions, 0 crashes.** 7 targets × {1, 2, 4, 8}
flipped bits × 10,000 trials each, every trial forked under an alarm and
classified against the shadow model.

| Profile | Metadata / allocation | Trials | Coverage | Silent | Crashes |
|---|---:|---:|---:|---:|---:|
| `fast` | 8 B | 240,000 | 74.13% | 41,022 | 0 |
| `hardened` (default) | 24 B | 280,000 | 100.00% | 0 | 0 |
| `paranoid` | 40 B | 280,000 | 100.00% | 0 | 0 |

Coverage is `detected / (detected + silent)` — of the flips that mattered, how
many were caught. Flips that landed in slack, padding or free space are
excluded, because there was nothing there to catch. `fast` carries no checksum
and no canary, so it detects far less; it is measured on the same sweep and its
cost is recorded rather than hidden.

**0 crashes** is the row that matters most, because it is the one promise every
profile makes unconditionally: no corrupted control word makes the allocator
read or write outside the arena. It has not always held. Two `fast` trials in
240,000 used to write a boundary tag about 115 MB past a 262 KB arena, and the
cause sat four steps upstream of the crash — a free block's boundary tag is the
only second copy of its extent when there is no header checksum, and the two
walks that write into blocks they find were not consulting it. Both trials are
now replayed by seed as tests under all three profiles, and
[docs/FAULT_MODEL.md](docs/FAULT_MODEL.md#where-that-promise-once-did-not-hold)
sets out the mechanism in full. Fixing it also took `fast`'s single-bit
`free_hdr` silent count from 160 in 10,000 to 0.

Throughput against glibc `malloc` on the same workload code, median of 11
pinned repetitions, 400,000 operations each, 64 MB arena:

| Workload | `hardened` ops/s | glibc ops/s | ratio | p50 | util% |
|---|---:|---:|---:|---:|---:|
| `seq_lifo` | 5,566,274 | 20,441,785 | 0.27× | 108 ns | 66.7 |
| `seq_fifo` | 4,386,148 | 20,299,790 | 0.22× | 113 ns | 66.7 |
| `random_size` | 3,648,712 | 10,626,431 | 0.34× | 157 ns | 97.4 |
| `churn` | 5,273,182 | 16,989,710 | 0.31× | 101 ns | 99.4 |
| `realloc_grow` | 4,663,288 | 2,757,089 | 1.69× | 128 ns | 100.0 |
| `fragmentation` | 3,700,471 | 13,695,435 | 0.27× | 117 ns | 94.3 |
| `validated_access` | 301,044 | 2,032,129 | 0.15× | 180 ns | 100.0 |

**This is three to five times slower than glibc on most workloads, and that is
the honest headline.** glibc has a per-thread cache in front of its free lists
and does no integrity checking at all; some of the difference is buying
something it does not provide, and some of it is simply glibc being very good.
`realloc_grow` is the one workload where the fixed arena and in-place growth
win outright. `validated_access` is the far outlier and is dominated by CRC32C
over the whole payload on every read — the cost of the feature, not of the
allocator.

Every number here is generated from a committed CSV in
[bench/results/](bench/results/), and `tools/check_readme_numbers.py` fails CI
if one appears that is not. [docs/RESULTS.md](docs/RESULTS.md) has the full
tables: all three profiles, latency percentiles, the counter overhead, and the
detection-latency curve.

## Architecture

```
                      caller-supplied arena, [lo, hi)
  +--------+--------------+--------+--------------------+--------+
  | block  | block        | block  | block              | block  |   the tiling
  +--------+--------------+--------+--------------------+--------+   is the list
     used       free         used          free            used
                  |                          |
                  +------------+-------------+
                               v
                     128 size-classed bins + a 128-bit bitmap
                     0..63   exact, MM_MIN_BLOCK + 16i
                     64..127 log-spaced, 4 per octave
                     a cache over the tiling, never the authority
```

One allocated block, under the default profile:

```
  +0    control word (8 B)     block_size | slack | QUARANTINED | PREV_IN_USE | IN_USE
  +8    hdr_crc | payload_crc  CRC32C over the word, the payload CRC,
                               the block's index and a per-arena secret
  +16   payload ...............................................
  +..   canary (8 B)           bound to the same index and secret
                               slack, from rounding up to the alignment
```

A **free** block spends its payload space on `next ^ secret`, `prev ^ secret`
and a boundary tag repeating its own extent, so stepping backwards is O(1) and
an allocated block pays nothing for it.

Blocks tile the arena exactly, always. Stepping forward is `b + block_size`;
there is no adjacency list that can fall out of step with the memory it
describes. The bins are a cache over that tiling, and anything finding them
inconsistent rebuilds from it and reports `MM_ERR_CORRUPT_LINKS`.

Because allocation is O(1), the allocator stops touching most of the arena — so
`mm_scrub` patrols it, a bounded walk resuming where it stopped, every 1024
allocator calls by default. What that costs is a curve rather than a constant,
and it is measured: damage to a free header or a link is found in ten to
fifteen calls whatever the patrol is set to, while a payload or canary flip is
the patrol's alone and with the patrol off is never found by traffic at all.

Three compile-time profiles select the layout:

| | `fast` | `hardened` (default) | `paranoid` |
|---|---:|---:|---:|
| header | 8 | 16 | 16 |
| canary / tail mirror | – / – | 8 / – | 8 / 16 |
| metadata per allocation | 8 | 24 | 40 |
| detects a header bit flip | no | yes | yes, **and repairs it** |

## Reproduce every number here

Ubuntu 22.04, gcc 11.4, CMake 3.22. `SHA` below is
`$(git rev-parse --short HEAD)`.

```bash
# Build and test
cmake --preset gcc-release
cmake --build --preset gcc-release -- -j$(nproc)
ctest --preset gcc-release --output-on-failure

# The throughput and space tables. --preset gcc-fast / gcc-paranoid for the
# other profiles, gcc-nostats to price the counters.
taskset -c 2 ./build/gcc-release/bin/bench \
  --ops 200000 --reps 11 --arena 67108864 \
  --git-sha "$SHA" --out bench/results/bench-hardened.csv

# The fault-injection matrix: 7 targets x {1,2,4,8} bits x 10,000 trials
./build/gcc-release/bin/faultinject \
  --trials 10000 --bits 1,2,4,8 --git-sha "$SHA" \
  --csv bench/results/faults-hardened.csv

# The detection-latency curve against the scrub interval
for s in 1 256 1024 4096 off; do
  ./build/gcc-release/bin/faultinject \
    --trials 2000 --bits 1,2 --scrub-interval "$s" \
    --git-sha "$SHA" --csv bench/results/scrub-hardened.csv
done

# Regenerate docs/RESULTS.md, and check the README against the CSVs
python3 tools/report.py
python3 tools/check_readme_numbers.py
```

[bench/results/README.md](bench/results/README.md) lists every committed CSV
with the command that produced it.
[docs/BENCHMARK_METHOD.md](docs/BENCHMARK_METHOD.md) sets out the timing
protocol: two clocks for two jobs, timer overhead measured and subtracted and
disclosed, a log-linear histogram with a bounded relative error, one discarded
warmup, medians over repetitions written out individually.

## Design

Full detail in [docs/DESIGN.md](docs/DESIGN.md),
[docs/BLOCK_LAYOUT.md](docs/BLOCK_LAYOUT.md) and
[docs/FAULT_MODEL.md](docs/FAULT_MODEL.md). Four decisions carry most of it.

**Payload integrity cannot survive a raw pointer.** An allocator that returns a
pointer has, at that instant, given up the ability to say anything about the
bytes behind it: the caller can write through it without telling anyone, and
any payload checksum is stale from the next store. So payload integrity is
offered where it can actually be maintained — `mm_read` verifies before copying
out, `mm_write` refreshes after copying in — and `mm_malloc` still returns an
ordinary pointer. The header, canary and boundary tags are protected either
way, because the allocator is their only writer. That is a property of
returning pointers at all, not a gap to be closed.

**A 32-bit CRC is enough, so there is no mirrored footer.** The header checksum
covers 32 bytes and detects every 1-, 2- and 3-bit error outright. A second
copy of the header adds no *detection* to that — it adds *repair*, which is the
one thing `paranoid` spends its extra 16 bytes on and the reason the other two
profiles carry no mirror at all. Mixing the block's index and a per-arena
secret into the checksum turns it from a bit-flip detector into a
block-confusion detector: a header copied verbatim from elsewhere in the arena,
correct in every field, fails because it was checksummed for another position.

**Benign flips are a separate bucket from silent ones.** Most flips into a
large arena land somewhere that never mattered. Counting those as failures
would understate the allocator badly and counting them as successes would
overstate it just as badly, so every trial carries a shadow model and only a
read that reported success and returned different bytes counts as silent. That
distinction is what makes a detection rate mean anything.

**O(1) allocation has to buy back the coverage it does not provide.** A linear
best-fit search revalidates every free block on every call: that is its cost,
and incidentally it is complete, continuous coverage of the arena for nothing.
Bins remove both together — a block nobody is using is a block nobody looks at
— and offering that trade without replacing the coverage would sell the whole
reliability argument for throughput. Hence the patrol, and hence detection
latency being a measured quantity with the scrub interval as a mandatory column
in every fault-injection CSV.

## Verification

| | |
|---|---|
| Unit and property tests | 9 suites, run under gcc and clang, Debug and Release, and all three profiles |
| Differential fuzzer | every operation checked against a shadow model, seeded so any failure replays exactly |
| Sanitizers | ASan, UBSan under gcc and clang; Valgrind memcheck over every test binary and a long fuzz run |
| Fault injection | gated in CI on the arena promise under all three profiles, and on the single-bit header cell where a checksum makes it decidable from theory. 56,000 trials per profile per push, plus the two seeds that once broke the promise replayed exactly |
| Coverage | gated at 90% lines on `src/`, not merely reported |
| Portability | Windows UCRT64 builds the core, tests, fuzzer and benchmark |

The single-bit header cell is worth singling out: a 32-bit CRC over the header
detects every single-bit change in it without exception, so under any profile
carrying one that cell **must** come out at 100% detection with zero silent
corruption. If it does not, the harness has found a defect rather than a
statistic. A cell theory already knows the answer to is worth more than a cell
that only has a measurement.

## What this is not

- **Not radiation hardening.** Real hardening is ECC memory, redundant
  hardware and physical shielding. Nothing in software stops a bit flipping;
  this notices afterwards. It models single-event upsets — it does not survive
  them at the hardware level.
- **Not thread-safe.** There is no lock and no per-thread cache, and none of
  the numbers above say anything about behaviour under concurrency.
- **Not a `malloc` replacement.** It manages a fixed caller-supplied arena and
  does not grow it. There is no `LD_PRELOAD` shim, and no timings against a
  real program — every workload here is a model of a program, not one.
- **Not able to repair a payload**, under any profile. If the flip landed in
  user data, the payload checksum reports it and the data is gone. Repair
  exists only for metadata, only under `paranoid`, and only while the mirror
  survives.
- **Not fast.** See the table above. The metadata is the point, and it is not
  free.
- **Not benchmarked on a quiet machine.** These were taken on a laptop under
  WSL2, where repeated runs move glibc's own figures by up to 1.5×. One
  significant figure is what the throughput numbers support; the
  fault-injection counts are seeded and deterministic and do not have this
  problem.

## Licence

MIT. See [LICENSE](LICENSE).
