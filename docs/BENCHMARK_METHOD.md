# How the numbers are measured

Every figure this project publishes comes from `bench/`, run as described here.
This document exists so that a number can be argued with: if the method is
wrong, the number is wrong, and both should be visible.

## Two clocks, for two different jobs

**Throughput** is measured with `CLOCK_MONOTONIC` (`QueryPerformanceCounter` on
Windows) around a whole batch of operations. This is the number to trust. It
measures elapsed time directly and does not care how long a clock read takes.

**Per-operation latency** is measured with the CPU timestamp counter
(`__rdtscp`). The allocator's fast path is tens of nanoseconds and a
`clock_gettime` call costs about as much again, so timing individual operations
with the wall clock would largely measure the clock.

The timestamp counter is only comparable across an interval if the CPU keeps it
running at a fixed rate. On Linux the harness reads `constant_tsc` and
`nonstop_tsc` from `/proc/cpuinfo` and records what it found in the CSV header
(`tsc_usable`, `tsc_flags_known`). Where the flags cannot be read — Windows —
`tsc_flags_known=0` is recorded rather than assuming the answer. **A latency
figure taken with `tsc_usable=0` should not be quoted.**

## Timer overhead is measured and disclosed

A back-to-back pair of counter reads is timed 20,000 times and the **minimum**
taken; anything above the minimum is interference, not the timer. That cost is
subtracted from every recorded sample and is written into the CSV header as
`timer_overhead_ns`, and into every row.

Quoting a p99 without saying what the measurement itself cost is not
meaningful when the thing being measured is of comparable size. The number is
published alongside the results for that reason.

## Latency histogram

Samples go into a fixed, pre-allocated log-linear histogram: within each power
of two, the value is indexed by its next 8 bits. Because the leading bit is
always set, 128 buckets are used per octave, giving a worst-case relative error
below 0.8%. Nothing is allocated while measuring.

Quantiles report the **floor** of the bucket they land in, so a reported
percentile is never higher than the true one, and never more than 0.8% below.
`min` and `max` are tracked exactly, as is the mean, which is accumulated from
raw values rather than from buckets.

`tests/test_bench_hist.c` checks this against distributions with known
percentiles. It is tested separately because every published latency depends on
it.

## Workloads

Frozen once written: changing a workload silently changes every number it has
ever produced.

| Name | Shape |
|---|---|
| `seq_lifo` | allocate a run of equal blocks, release newest first |
| `seq_fifo` | same, release oldest first — exposes coalescing behaviour |
| `random_size` | 90% at most 128 B, ~10% log-uniform 8–4096 B, 0.1% at 64 KB–1 MB; released in shuffled order |
| `churn` | steady population of 512 blocks; every allocation preceded by a release |
| `realloc_grow` | vector-style growth, ×1.5 each step |
| `fragmentation` | interleaved small and large, large ones reclaimed, then medium requests — measures how much of the arena survives as usable space |
| `validated_access` | integrity-checked reads and writes at 8 B, 64 B, 1 KB and 64 KB |

Each is seeded, so the same seed performs the same sequence of requests every
time.

## Protocol

- One discarded warmup run, then repetitions written individually to the CSV.
- **Report the median and show the spread.** Aggregation is deliberately left to
  the reporting step: raw repetitions are what let a reader see variance rather
  than trust a single figure.
- Pinned with `taskset -c 2`.
- The CSV header records CPU, kernel, compiler, date, git SHA, TSC status,
  timer overhead, and whether counters were compiled in.

## Comparison against the system allocator

Both allocators are driven through the same `bench_alloc` interface by the same
workload code, so no difference can come from the driver.

The comparison is still not like-for-like, and the difference matters:

- The benchmark drives this allocator over a fixed caller-supplied arena.
  glibc's `malloc` requests memory from the kernel and grows. (The allocator
  can now grow too, and does under the shim — but the benchmark deliberately
  does not use that, so the arena parameter stays a parameter and the numbers
  stay comparable with every earlier run.)
- glibc has a per-thread cache in front of its free lists.
- This allocator validates integrity metadata on operations where glibc does no
  checking at all. Some of the cost being measured buys something glibc does
  not provide.

None of that makes glibc a bad reference point — it makes it the *only* honest
one, provided the caveats travel with the number.

## The second measurement: real programs under LD_PRELOAD

Everything above measures a workload written to be measured. That is a real
weakness and no amount of care inside the harness fixes it, because the person
who chose the allocation pattern is the person who wrote the allocator.

`tools/preload_bench.py` is the answer to it. It `LD_PRELOAD`s the shim into
programs that know nothing about any of this — `ls`, `git`, `grep`, a Python
interpreter, a C compiler — and times the whole process against the same
program on glibc. Nobody chose those allocation patterns to suit anything here.

The method differs from the one above in ways that follow from the instrument:

- **Wall time of a whole process**, not per-operation timings. Fork, exec,
  dynamic linking and the program's own work are all inside the number.
  Allocation is a minority of it for every program in the set.
- **Allocators alternate repetition by repetition**, not one after the other,
  so that a machine drifting during the run does not land entirely on whichever
  went second. The page cache is warmed before the first timed repetition.
- **The heap check is run separately, afterwards.** `mm_check_heap` walks every
  block in the arena, and doing that inside a timed run would charge the
  allocator for a diagnostic the program never asked for. Each program is run
  once more with `MARS_SHIM_CHECK` set, and the pass and fail counts go into
  the CSV's `#` block.
- **Output is discarded and the exit code is recorded**, because a program that
  failed early would otherwise look like a fast one.

What this measurement can and cannot say is the important part. It can say the
allocator runs real software correctly, that the software produces identical
output, and that the heap it leaves behind is consistent. On cost it says much
less than it appears to: **a ratio near 1.0 means the allocator disappeared
into the noise of everything else the process did, not that it is as fast as
glibc.** For per-operation cost, read the microbenchmarks, where it is visible
and where it is three to five times glibc's.

## Environment

Development and measurement happen under WSL2 on an otherwise idle Windows
host. That is not a quiet benchmarking environment: expect tail latencies to
carry scheduler noise that a bare-metal machine would not show. This is why
throughput is taken from wall-clock batches, why the median of several
repetitions is reported, and why the spread is published rather than hidden.

## What these numbers are not

They are not a claim about behaviour under concurrency: the allocator is
single-threaded, and so is the shim. And a throughput figure says nothing about
whether the memory handed back was correct — that is what the test suite, the
fuzzer, and the fault injector are for.

The microbenchmarks are also not a claim about real programs; a workload is a
model of a program, not one. That gap is what the preload runs above exist to
close, and they close it only partly: they establish that real programs run
correctly and leave a consistent heap, and they are too noisy an instrument to
say much about cost.
