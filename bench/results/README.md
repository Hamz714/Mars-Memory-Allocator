# Recorded measurements

Every number quoted anywhere in this repository comes from a file in here. A
figure with no CSV behind it is a figure nobody can check, reproduce or
disagree with, so it does not get written down —
`tools/check_readme_numbers.py` enforces that on the README in CI, and
`tools/report.py` turns these files into [`docs/RESULTS.md`](../../docs/RESULTS.md).

Each file opens with a `#` comment block recording the machine, kernel,
compiler, commit and run parameters it was taken on. **Anything reading these
files has to skip lines beginning with `#`.** Comparing rows from different
files means reading those blocks first.

**One significant figure is what the throughput numbers support.** They were
taken on a laptop under WSL2, where repeated runs move the *system* allocator's
own figures by as much as 1.5× — so a difference of tens of percent between two
runs is the machine, not the code. Ratios that hold across every run, and
changes measured in orders of magnitude, are what these files are good for. The
fault-injection numbers do not have this problem: they are counts from seeded,
deterministic trials and reproduce exactly.

## The current pinned run

Configure with the matching preset first, e.g.
`cmake --preset gcc-release && cmake --build --preset gcc-release`. `SHA` below
is `$(git rev-parse --short HEAD)`.

| File | Preset | What it is | How to reproduce |
|---|---|---|---|
| `bench-fast.csv` | `gcc-fast` | Throughput, latency and space, mars against glibc, on the seven frozen workloads | `taskset -c 2 bench --ops 200000 --reps 11 --arena 67108864 --git-sha SHA --out FILE` |
| `bench-hardened.csv` | `gcc-release` | the same, default profile | as above |
| `bench-paranoid.csv` | `gcc-paranoid` | the same, tail mirror | as above |
| `bench-hardened-nostats.csv` | `gcc-nostats` | the same again with `MARS_STATS` off, to price the counters | as above |
| `faults-fast.csv` | `gcc-fast` | The full injection matrix: 7 targets × {1,2,4,8} bits × 10,000 trials | `faultinject --trials 10000 --bits 1,2,4,8 --git-sha SHA --csv FILE` |
| `faults-hardened.csv` | `gcc-release` | the same | as above |
| `faults-paranoid.csv` | `gcc-paranoid` | the same | as above |
| `scrub-fast.csv` | `gcc-fast` | Detection rate and detection latency against the scrub interval | `for s in 1 256 1024 4096 off; do faultinject --trials 2000 --bits 1,2 --scrub-interval $s --git-sha SHA --csv FILE; done` |
| `scrub-hardened.csv` | `gcc-release` | the same | as above |
| `scrub-paranoid.csv` | `gcc-paranoid` | the same | as above |

The benchmark writes one row per repetition and does not aggregate. That is
deliberate: raw repetitions are what let a reader see the variance rather than
trust a single figure, and it means the median and the inter-quartile range in
`docs/RESULTS.md` can be recomputed and argued with.

`faultinject` **appends** when the file already exists and writes the `#` block
only when creating it, which is what lets the five scrub intervals accumulate
into one table. The scrub interval is a per-row column rather than a header
field for exactly that reason.

## Earlier reference runs

Kept so that the cost of a change can still be priced after the fact. They were
taken on the same machine, at fewer operations and repetitions, and are **not**
comparable row-for-row with the pinned run above.

| File | What it is |
|---|---|
| `phase05-freelists.csv` | Throughput and utilisation when the size-classed bins landed: `bench --ops 20000 --reps 9 --arena 33554432` |
| `scrub-sweep.csv` | The first scrub-interval curve, hardened only, 200 trials per cell |

## Reading the fault-injection files

`detection_pct` is **coverage**: `detected / (detected + undetected_silent)` —
of the flips that actually mattered, how many were caught. `undetected_benign`
is excluded from it, because a flip that landed in slack, padding or free space
had nothing there to catch, and counting those as either successes or failures
would misstate the allocator in opposite directions.

`crash` and `timeout` are separate columns. Every profile is meant to promise
never to read or write outside the arena whatever a corrupted control word
says, so a `crash` is that promise broken; a `timeout` is a traversal that was
still running when the five-second alarm fired, which is a different defect.
Both columns are zero in all three files. An earlier `faults-fast.csv` carried
two crashes; that was a defect rather than a cost of the `fast` trade, and
[FAULT_MODEL.md](../../docs/FAULT_MODEL.md#where-that-promise-once-did-not-hold)
records the mechanism, the fix and the two seeds that now replay it as tests.

`latency_mean_ops` is the mean number of allocator calls between a bit flip and
the allocator first reporting it, over ordinary traffic that never touches the
damaged block. It is **censored** at the 4,096-call measurement window:
`latency_n` says how many trials were detected inside it, and when that falls
short of `trials` the mean is a lower bound rather than an estimate.

Two groups of targets behave completely differently, and that is the point.
`free_hdr`, `links` and `free_footer` sit on the allocation path, so
validate-on-touch finds damage to them in ten to fifteen calls whatever the
patrol is set to. Damage to an allocated block's header, canary or payload is
found only by the patrol, and with the patrol off it is not found at all by
traffic alone — `latency_n` of 0. That is the cost of O(1) allocation, priced.

## Reading the `fast` profile

`fast` carries no header checksum and no canary, so it detects less, and the
files record how much less rather than omitting it. Its `payload` cells are
essentially all silent — there is no payload checksum under that profile — and
its `alloc_hdr` cells carry silent corruption the other two profiles hold at
zero, because an allocated block under this profile has no second copy of
anything. (`any` flips anywhere in the arena, so most of its silent count is
payload hits reached by another route.) Leaving those cells out would hide the
cost of the trade instead of pricing it, which is why
CI gates `fast` on the arena promise and records its detection numbers rather
than gating them.

The `free_hdr` cells are worth a second look: they are at zero silent under all
three profiles, which was not true of `fast` before the arena-promise fix. A
free block's boundary tag repeats its extent, so `fast` has a second copy of
that one number even without a checksum — and once the walks that write into
blocks started requiring the two to agree, the 245 silent `free_hdr` trials went
with the crashes.
