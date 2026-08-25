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
| `bench-hardened-nolock.csv` | `gcc-nolock` | and again with `MARS_LOCK` off, to price the lock | as above |
| `bench-hardened-repeat.csv` | `gcc-release` | **the same build as `bench-hardened.csv`, run again minutes later.** The machine's own run-to-run movement, which is what the two comparisons above are judged against | as above |
| `preload-fast.csv` | `gcc-fast` | Wall time of nine real programs under `LD_PRELOAD`, against the same programs on glibc | `taskset -c 2 python3 tools/preload_bench.py --preload build/gcc-fast/bin/libmars_preload.so --probe build/gcc-fast/bin/calloc_probe --profile fast --reps 11 --git-sha SHA --out FILE` |
| `preload-hardened.csv` | `gcc-release` | the same, default profile | as above, with `gcc-release` and `--profile hardened` |
| `preload-paranoid.csv` | `gcc-paranoid` | the same, tail mirror | as above, with `gcc-paranoid` and `--profile paranoid` |
| `faults-fast.csv` | `gcc-fast` | The full injection matrix: 7 targets × {1,2,4,8} bits × 10,000 trials | `faultinject --trials 10000 --bits 1,2,4,8 --git-sha SHA --csv FILE` |
| `faults-hardened.csv` | `gcc-release` | the same | as above |
| `faults-paranoid.csv` | `gcc-paranoid` | the same | as above |
| `scrub-fast.csv` | `gcc-fast` | Detection rate and detection latency against the scrub interval | `for s in 1 256 1024 4096 off; do faultinject --trials 2000 --bits 1,2 --scrub-interval $s --git-sha SHA --csv FILE; done` |
| `scrub-hardened.csv` | `gcc-release` | the same | as above |
| `scrub-paranoid.csv` | `gcc-paranoid` | the same | as above |
| `threads-arena.csv` | `gcc-release` | Total throughput of `mt_churn` and `producer_consumer` at 1, 2, 4 and 8 threads, mars against glibc, with one arena per thread | `bench_mt --ops 200000 --reps 11 --threads 1,2,4,8 --git-sha SHA --out FILE` |
| `threads-global.csv` | `gcc-lock-global` | the same, with one mutex around every entry point instead | as above |

**The four `bench-hardened*` files were taken in one alternating pass**, in the
order `hardened`, `nostats`, `nolock`, `hardened` again — not one after another
in whatever order was convenient. The counter and lock tables in
`docs/RESULTS.md` are the only comparisons in this directory that hold two
*different builds* against each other, and they are the ones a drifting machine
can invent a result for. Running them minutes apart inside one window, and
running the first build a second time at the end of it, is what makes the
difference between them checkable against the instrument's own error.

**That error is larger than it looks.** Two runs of identical code, on an idle
pinned core, differ by up to 43% on this machine — `bench-hardened-repeat.csv`
is that measurement, and `docs/RESULTS.md` prints it as a column beside every
build-to-build comparison. The inter-quartile range of a single run is *not*
the same quantity and is several times smaller: eleven repetitions inside one
process agree far more closely than two processes minutes apart do.

The benchmark writes one row per repetition and does not aggregate. That is
deliberate: raw repetitions are what let a reader see the variance rather than
trust a single figure, and it means the median and the inter-quartile range in
`docs/RESULTS.md` can be recomputed and argued with.

`faultinject` **appends** when the file already exists and writes the `#` block
only when creating it, which is what lets the five scrub intervals accumulate
into one table. The scrub interval is a per-row column rather than a header
field for exactly that reason.

**The `faults-*.csv` files are not re-measured when they do not change, and
whether they change is checked rather than assumed.** Every trial is seeded and
forked, so the matrix is deterministic: re-running it against new code either
reproduces the committed file byte for byte or it does not, and which of those
happened is a result in itself. The three matrices here were re-run after
per-thread arenas landed and came back identical, which is the evidence that
nothing about detection moved — a claim that would otherwise have rested on
somebody's reading of the diff.

## Reading the `threads-*.csv` files

These are the only files here that are a **curve** rather than a set of
independent cells, and they are read down a column rather than across. `ops` is
the total across all threads and scales with the thread count by construction —
each thread performs `ops_per_thread` timed operations whatever T is — so
`ops_per_sec` is total throughput and an allocator that scales perfectly makes
it a straight line. What matters is that shape, not any single row.

One file per **locking strategy**, which is the variable the whole set exists to
compare. It is in the `# lock=` header field — which every file in this
directory now carries — as well as in a per-row column, so that two files
cannot be confused for each other. `MARS_LOCK` is a CMake option; the benchmark
records what the library it was linked against was actually built with, not
what anyone intended.

**Taken without `taskset`**, unlike everything else in this directory. Pinning a
thread-scaling run to one core would measure the scheduler. The machine has
**4 physical cores and 8 hardware threads**, so the 8-thread rows put two
threads on a core and cannot double the 4-thread rows even for code that scales
perfectly — the 1→4 part of each column is the part that is about the
allocator.

`mars` is driven through a **growable** arena here rather than a
caller-supplied buffer, which is recorded in the `# arena=growable` header
field. A fixed buffer is one region and cannot be divided between threads, so
measuring per-thread scaling against one would measure the fallback rather than
the design. Under a global lock the two are the same, which is what keeps the
files comparable across strategies.

The glibc columns are a scale rather than a target: glibc has a per-thread
cache in front of its free lists and does no integrity checking at all. Its
*speedup* column is the useful part — it says what this machine and this
workload are capable of, which is what makes a flat column beside it a
statement about the allocator rather than about the benchmark. Its
`producer_consumer` row is worth reading too: it does not scale cleanly either,
because cross-thread frees cost glibc something as well.

`alloc_failures` counts allocations that came back NULL. It is zero in every
committed row; anything else means the arena ran out and the row is measuring
exhaustion rather than throughput.

## Reading the `preload-*.csv` files

These are the only numbers here that come from software nobody wrote for this
project, and they are read differently from everything else.

`wall_ns` is the wall time of a **whole process**: fork, exec, dynamic linking,
the program's own work, and its allocation. Allocation is a small part of that
for every program in the set, which is the point — it is what makes the
comparison a statement about real software — and also the limitation. **A ratio
near 1.0 means the allocator disappeared into the noise, not that it is as fast
as glibc.** The per-operation cost is in `bench-*.csv`, where it is visible.

The harness alternates allocators repetition by repetition rather than running
all of one and then all of the other, and warms the page cache before the first
timed repetition, so that a machine drifting mid-run does not land on whichever
allocator went second. It still cannot make process wall time a quiet
instrument: `git_log_stat` reads git objects off a Windows filesystem through
WSL and its inter-quartile range is wide enough to swallow anything the
allocator does. It is kept because dropping the noisiest program because it is
noisy is how a benchmark set becomes flattering.

`max_rss_kb` is the child's peak resident set, from `wait4`. It is the honest
place to look for what 24 bytes per allocation costs on a real program, and
`python_dict` is the row where it is large enough to see.

The `calloc_*` rows carry a **third** allocator, `mars_nofresh`: the same build
run with `MARS_SHIM_NOFRESH=1`, which makes `calloc` memset every byte it hands
back rather than trusting pages the kernel has just supplied. The pair is the
measurement of that one optimisation. `calloc_4mb_x200` is the case it exists
for -- each allocation gets a mapping of its own, straight from the kernel and
already zero -- and `calloc_4kb_x200000` is the case it cannot help, where
every block is recycled arena memory and the memset has to happen. Both are
recorded, because a shortcut is only worth quoting next to the case where it
does nothing.

`exit_code` is checked rather than decorative: the programs are run with output
discarded, so a program that failed would otherwise look like a fast one. Each
program is additionally run once more, outside the timing, with
`MARS_SHIM_CHECK` pointing at a log, and `mm_check_heap()` walks the whole
arena at exit. The `# heap_checks_passed=` and `# heap_checks_failed=` fields
record the outcome, and the harness refuses to write a file where anything
failed.

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
