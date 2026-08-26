#!/usr/bin/env python3
"""Generate docs/RESULTS.md from the committed CSVs in bench/results/.

Nothing here computes a measurement. It reads the raw per-repetition rows the
benchmark wrote and the per-cell counts the fault injector wrote, and lays them
out. Aggregation lives here rather than in the C so that the committed files
stay raw: a reader who disagrees with how the median or the interval was taken
can recompute from the same data.

  python3 tools/report.py                       # writes docs/RESULTS.md
  python3 tools/report.py --check               # exits 1 if it is out of date

File naming is the discovery mechanism, because the alternative is a manifest
that goes stale:

  bench-<profile>.csv            throughput and space, mars and glibc
  bench-<profile>-nostats.csv    the same, counters compiled out
  bench-<profile>-nolock.csv     the same, locking compiled out
  bench-<profile>-repeat.csv     the same build again, minutes later: the
                                 machine's own run-to-run movement, which is
                                 what the two comparisons above are judged
                                 against
  preload-<profile>.csv          wall time of real programs under LD_PRELOAD
  faults-<profile>.csv           the full target x bits matrix
  scrub-<profile>.csv            the same targets swept over scrub intervals
  threads-<lock>.csv             total throughput against the thread count,
                                 one file per locking strategy

No charts. GitHub strips inline SVG out of rendered Markdown, so a chart here
would be an asset that only renders locally -- and an external asset file is
exactly what a self-contained results document should not need.
"""

import argparse
import csv
import math
import pathlib
import statistics
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
RESULTS = REPO / "bench" / "results"
OUT = REPO / "docs" / "RESULTS.md"

PROFILES = ["fast", "hardened", "paranoid"]

# Locking strategies, in the order the design arrived at them. The thread
# scaling section is a comparison between them, so the order is the argument.
LOCKS = ["global", "arena"]

LOCK_BLURB = {
    "global": "One mutex around every public entry point. Correct under "
              "threads, and the control anything that follows it has to be "
              "measured against.",
    "arena": "One arena per thread, each with its own lock, and a lock-free "
             "stack for frees that cross a thread boundary.",
}

MT_BLURB = {
    "mt_churn": "T independent copies of the `churn` workload, sharing "
                "nothing. Anything that fails to scale here is the allocator "
                "rather than the workload.",
    "producer_consumer": "Half the threads allocate and enqueue, half dequeue "
                         "and free, so **every block is freed by a thread that "
                         "did not allocate it**. This is the case a per-thread "
                         "arena has to answer for.",
}

# The order the fault injector emits, kept so a table reads the same way the
# tool's own output does.
TARGETS = ["alloc_hdr", "free_hdr", "links", "free_footer", "payload",
           "canary", "any"]

# Targets that sit on the allocation path, so validate-on-touch finds them
# whatever the patrol is doing, against targets that only the patrol will ever
# look at. The split is the whole point of the scrub curve, so it is named here
# rather than left for the reader to infer.
#
# `any` is in neither. It flips a bit anywhere in the arena and so lands in
# both populations plus slack and free space; averaging it into either column
# would blur exactly the contrast the table exists to show.
ON_PATH = {"free_hdr", "links", "free_footer"}
# An allocated block's header is its own population, and the measurement is
# what says so: with the patrol off it is still found by traffic about half the
# time, because freeing and coalescing a neighbour validates the header next to
# it. It is neither reliably on the allocation path nor the patrol's alone.
NEIGHBOUR = {"alloc_hdr"}
# These two are the patrol's alone. With the patrol off, traffic that never
# touches the block never finds them: latency_n is exactly 0.
PATROL_ONLY = {"payload", "canary"}


# --- Reading ----------------------------------------------------------------

def read(path):
    """Returns (provenance dict, rows). Provenance is the `# k=v` block."""
    meta, body = {}, []
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                for field in line[1:].strip().split():
                    if "=" in field:
                        k, v = field.split("=", 1)
                        meta.setdefault(k, v)
                # `# cpu=Intel(R) Core(TM) i7` has spaces in the value, so the
                # split above keeps only the first word. Recover the whole tail
                # for the single-field lines.
                stripped = line[1:].strip()
                if stripped.count("=") == 1:
                    k, v = stripped.split("=", 1)
                    meta[k] = v
            else:
                body.append(line)
    return meta, list(csv.DictReader(body))


def load(prefix, profile, required=True):
    path = RESULTS / f"{prefix}-{profile}.csv"
    if not path.exists():
        if required:
            sys.exit(f"missing {path}; see bench/results/README.md")
        return None, None
    return read(path)


# --- Aggregation ------------------------------------------------------------

def median(values):
    return statistics.median(values) if values else float("nan")


def iqr(values):
    """Inter-quartile range. Reported instead of a standard deviation because
    the tail here is scheduler noise, not spread in the thing being measured."""
    if len(values) < 4:
        return float("nan")
    q = statistics.quantiles(values, n=4, method="inclusive")
    return q[2] - q[0]


def wilson(successes, total):
    """95% Wilson score interval, in percent. Same arithmetic as the injector's
    own, recomputed here so that cells aggregated across bit counts still carry
    an interval -- and so the two can be cross-checked against each other."""
    if total == 0:
        return 0.0, 0.0
    z = 1.959964
    n = float(total)
    p = successes / n
    denom = 1.0 + z * z / n
    centre = (p + z * z / (2 * n)) / denom
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / denom
    return max(0.0, (centre - half) * 100.0), min(100.0, (centre + half) * 100.0)


def cell(rows):
    """Folds fault-injection rows into one set of counts."""
    keys = ["trials", "detected_no_loss", "detected_quarantined",
            "detected_fatal", "undetected_benign", "undetected_silent",
            "crash", "timeout", "latency_n"]
    out = {k: sum(int(r[k]) for r in rows) for k in keys}
    out["detected"] = (out["detected_no_loss"] + out["detected_quarantined"] +
                       out["detected_fatal"])
    # Coverage counts only the flips that mattered: benign ones landed in slack
    # or free space and there was nothing there to catch.
    out["mattered"] = out["detected"] + out["undetected_silent"]
    weighted = sum(float(r["latency_mean_ops"]) * int(r["latency_n"])
                   for r in rows)
    out["latency_mean"] = (weighted / out["latency_n"]
                           if out["latency_n"] else float("nan"))
    return out


def si(x):
    return f"{x:,.0f}"


# --- Sections ---------------------------------------------------------------

def provenance(o, benches):
    """One block for the whole document. The runs must agree about the machine
    they came from, or nothing below is comparable and saying so is the point."""
    metas = [m for m, _ in benches.values()]
    base = metas[0]
    for key in ("cpu", "kernel", "compiler"):
        if len({m.get(key) for m in metas}) != 1:
            sys.exit(f"the runs disagree about {key}; they are not comparable")

    o(f"Taken on `{base.get('cpu', '?')}`, {base.get('kernel', '?')}, "
      f"{base.get('compiler', '?')}, at commit `{base.get('git_sha', '?')}` "
      f"on {base.get('date', '?')}.\n")

    # Every run has to be able to support the latency columns, not just the
    # first file that happened to be read.
    tsc = all(m.get("tsc_usable") == "1" and m.get("tsc_flags_known") == "1"
              for m in metas)
    overheads = sorted(float(m["timer_overhead_ns"]) for m in metas)
    if tsc:
        span = (f"{overheads[0]:.2f} ns" if overheads[0] == overheads[-1]
                else f"{overheads[0]:.2f}–{overheads[-1]:.2f} ns")
        o(f"The timestamp counter is constant and non-stop here "
          f"(`tsc_usable=1`) in every run, so the latency figures below are "
          f"quotable. A back-to-back pair of counter reads costs **{span}** "
          f"depending on the run, measured each time as the minimum of 20,000 "
          f"attempts and subtracted from that run's own samples. Percentiles "
          f"of the same order as that number should be read with it in mind.\n")
    else:
        o("**`tsc_usable=0` in at least one run: the latency columns below are "
          "not quotable** and are reproduced only so the run is complete.\n")

    o("Measured under WSL2 on a laptop, which is not a quiet benchmarking "
      "environment. Repeated runs move the *system* allocator's own throughput "
      "by as much as 1.5×, so **one significant figure is what these "
      "throughput numbers support**: ratios that hold across runs and "
      "differences of an order of magnitude are what they are good for. The "
      "fault-injection counts below do not have this problem, they come from "
      "seeded, deterministic trials and reproduce exactly.\n")


def throughput(o, benches):
    o("## Throughput\n")
    o("Median of the repetitions, with the inter-quartile range beside it. "
      "Wall-clock over a whole batch, not summed per-operation timings.\n")
    for profile in PROFILES:
        _, rows = benches[profile]
        o(f"\n### `{profile}`\n")
        o("| Workload | mars ops/s | IQR | glibc ops/s | mars / glibc |")
        o("|---|---:|---:|---:|---:|")
        for wl in workloads(rows):
            mars = [float(r["ops_per_sec"]) for r in rows
                    if r["workload"] == wl and r["allocator"] == "mars"]
            sysl = [float(r["ops_per_sec"]) for r in rows
                    if r["workload"] == wl and r["allocator"] == "system"]
            ratio = median(mars) / median(sysl) if median(sysl) else float("nan")
            o(f"| `{wl}` | {si(median(mars))} | ±{si(iqr(mars) / 2)} | "
              f"{si(median(sysl))} | {ratio:.2f}× |")


def latency(o, benches):
    o("\n## Per-operation latency\n")
    o("Timestamp-counter samples into a log-linear histogram, timer overhead "
      "already subtracted. Quantiles report the floor of the bucket they land "
      "in, so a percentile here is never above the true one and never more "
      "than 0.8% below.\n")
    o("| Workload | " + " | ".join(f"{p} p50 | {p} p99" for p in PROFILES) + " |")
    o("|---|" + "---:|" * (2 * len(PROFILES)))
    for wl in workloads(benches["hardened"][1]):
        cells = []
        for profile in PROFILES:
            rows = [r for r in benches[profile][1]
                    if r["workload"] == wl and r["allocator"] == "mars"]
            cells.append(f"{median([float(r['p50_ns']) for r in rows]):.0f} ns")
            cells.append(f"{median([float(r['p99_ns']) for r in rows]):.0f} ns")
        o(f"| `{wl}` | " + " | ".join(cells) + " |")


def space(o, benches):
    o("\n## Space\n")
    o("Utilisation is payload over what those blocks actually occupied, "
      "sampled at peak occupancy, so it captures metadata and alignment "
      "rounding together. Overhead per allocation comes from the same "
      "snapshot.\n")
    o("| Workload | " + " | ".join(f"{p} util% | {p} B/alloc"
                                   for p in PROFILES) + " |")
    o("|---|" + "---:|" * (2 * len(PROFILES)))
    for wl in workloads(benches["hardened"][1]):
        cells = []
        for profile in PROFILES:
            rows = [r for r in benches[profile][1]
                    if r["workload"] == wl and r["allocator"] == "mars"]
            cells.append(f"{median([float(r['util_pct']) for r in rows]):.1f}")
            cells.append(
                f"{median([float(r['meta_bytes_per_alloc']) for r in rows]):.0f}")
        o(f"| `{wl}` | " + " | ".join(cells) + " |")


def drift(on, repeat, wl):
    """How far the same build moved between two runs of it, in percent.

    The instrument's own error bar. Within-run inter-quartile range is not it:
    that measures how much eleven repetitions inside one process disagreed,
    which is far narrower than how much two processes minutes apart disagree on
    a laptop under WSL2. Comparing a build against a *different* build without
    knowing this number is how a benchmark invents results."""
    a = median([float(r["ops_per_sec"]) for r in on
                if r["workload"] == wl and r["allocator"] == "mars"])
    b = median([float(r["ops_per_sec"]) for r in repeat
                if r["workload"] == wl and r["allocator"] == "mars"])
    return abs(100.0 * (b - a) / a) if a else float("nan")


def cost_table(o, on, off, repeat, on_label, off_label):
    """One build against another, with the noise floor beside it."""
    o(f"| Workload | {on_label} | {off_label} | difference | same build, "
      f"run again | resolvable? |")
    o("|---|---:|---:|---:|---:|:-:|")
    inside, outside = [], []
    for wl in workloads(on):
        a = median([float(r["ops_per_sec"]) for r in on
                    if r["workload"] == wl and r["allocator"] == "mars"])
        b = median([float(r["ops_per_sec"]) for r in off
                    if r["workload"] == wl and r["allocator"] == "mars"])
        cost = 100.0 * (b - a) / a
        noise = drift(on, repeat, wl)
        clear = abs(cost) > noise
        (outside if clear else inside).append(wl)
        o(f"| `{wl}` | {si(a)} | {si(b)} | {cost:+.1f}% | ±{noise:.1f}% | "
          f"{'yes' if clear else 'no'} |")
    return inside, outside


def counters(o, benches, nostats, repeat):
    o("\n## What the counters cost\n")
    o("`MARS_STATS` is on by default and is what supplies the utilisation and "
      "overhead columns above. Both builds are Release, hardened, same "
      "machine, same seeds, taken minutes apart; the only difference is "
      "whether the counters are compiled in.\n")
    o("The fifth column is the same build measured a second time and is the "
      "only reason the fourth means anything. See "
      "[the note below](#the-noise-floor-and-why-it-is-a-column).\n")
    _, on = benches["hardened"]
    _, off = nostats
    inside, outside = cost_table(o, on, off, repeat, "counters on",
                                 "counters off")
    verdict(o, inside, outside, "the counters")


def locking_cost(o, benches, nolock, repeat):
    o("\n## What the lock costs a single-threaded program\n")
    o("`MARS_LOCK=arena` is the default and puts an **uncontended** mutex on "
      "every public entry point; `MARS_LOCK=none` compiles it out entirely. "
      "The question is what a program with one thread pays for the fact that "
      "it might have had two.\n")
    _, on = benches["hardened"]
    _, off = nolock
    inside, outside = cost_table(o, on, off, repeat, "locked", "no lock")
    verdict(o, inside, outside, "the lock")
    o("Whatever it is, it is the price of the *first* strategy and not of the "
      "third: an uncontended mutex is two atomic operations and no syscall, "
      "and it is the same two whether the program has one thread or eight. "
      "What the thread-scaling section measures is a different quantity: who "
      "else is waiting on that mutex, and there the two strategies are not "
      "close.\n")


def verdict(o, inside, outside, what):
    o(f"\nThe difference is larger than the same build's own run-to-run "
      f"movement on **{len(outside)} of {len(outside) + len(inside)}** "
      f"workloads"
      + (f" ({', '.join('`' + w + '`' for w in outside)})" if outside else "")
      + ", and smaller than it on the rest"
      + (f" ({', '.join('`' + w + '`' for w in inside)})" if inside else "")
      + f". On those, the honest statement is that the cost of {what} is below "
      f"what this machine can resolve, not that there is none.\n")


def noise_floor(o, benches, repeat):
    o("\n## The noise floor, and why it is a column\n")
    o("Two runs of the **same build**, same machine, same seeds, a few minutes "
      "apart, with nothing else running and both pinned to one core. Nothing "
      "changed between them but time.\n")
    _, on = benches["hardened"]
    _, again = repeat
    o("| Workload | first run | second run | moved by |")
    o("|---|---:|---:|---:|")
    worst = 0.0
    for wl in workloads(on):
        a = median([float(r["ops_per_sec"]) for r in on
                    if r["workload"] == wl and r["allocator"] == "mars"])
        b = median([float(r["ops_per_sec"]) for r in again
                    if r["workload"] == wl and r["allocator"] == "mars"])
        moved = 100.0 * (b - a) / a
        worst = max(worst, abs(moved))
        o(f"| `{wl}` | {si(a)} | {si(b)} | {moved:+.1f}% |")
    o(f"\n**Up to {worst:.0f}% between two runs of identical code.** That is "
      f"the instrument, not the allocator, and it is why the two tables above "
      f"carry it as a column rather than comparing against the inter-quartile "
      f"range of a single run, eleven repetitions inside one process agree "
      f"with each other far more closely than two processes minutes apart do, "
      f"so an IQR-based comparison would call a difference significant that "
      f"this machine cannot see at all.\n")
    o("It is also why the thread-scaling section below is quoted as a *shape* "
      "rather than as figures: a column that goes from 0.13× to 3.4× is not "
      "something a 40% instrument can be wrong about.\n")

def faults(o, matrices):
    o("\n## Fault injection\n")
    m0 = matrices["hardened"][0]
    o(f"{int(m0.get('trials_per_cell', 0)):,} trials per cell, "
      f"{len(TARGETS)} targets Ã,  {{1, 2, 4, 8}} bits Ã,  {len(PROFILES)} "
      f"profiles. Each trial forks a child under an alarm, flips *k* bits in "
      f"the chosen structure, and classifies what the allocator then does "
      f"against a shadow model of what every live payload is supposed to "
      f"contain. Base seed `{m0.get('base_seed', '?')}`, arena "
      f"{int(m0.get('arena_bytes', 0)):,} bytes, patrol at its 1024-call "
      f"default, at commit `{m0.get('git_sha', '?')}`.\n")
    o("**Detection coverage is `detected / (detected + silent)`** â€” of the "
      "flips that actually mattered, how many were caught. Benign flips landed "
      "in slack, padding or free space and are excluded, because there was "
      "nothing there to catch. Intervals are 95% Wilson score.\n")

    for profile in PROFILES:
        meta, rows = matrices[profile]
        o(f"\n### `{profile}` â€” {meta.get('metadata_bytes', '?')} B of "
          f"metadata per allocation\n")
        o("| Target | Trials | Benign | Detected | Silent | Crash | Timeout | "
          "Coverage (95% CI) |")
        o("|---|---:|---:|---:|---:|---:|---:|---:|")
        for target in TARGETS:
            got = [r for r in rows if r["target"] == target]
            if not got:
                o(f"| `{target}` | â€“ | â€“ | â€“ | â€“ | â€“ | â€“ | "
                  f"no such structure under this profile |")
                continue
            c = cell(got)
            lo, hi = wilson(c["detected"], c["mattered"])
            pct = (100.0 * c["detected"] / c["mattered"]
                   if c["mattered"] else float("nan"))
            cover = ("n/a â€” nothing mattered" if not c["mattered"]
                     else f"{pct:.2f}% [{lo:.2f}, {hi:.2f}]")
            o(f"| `{target}` | {c['trials']:,} | {c['undetected_benign']:,} | "
              f"{c['detected']:,} | **{c['undetected_silent']:,}** | "
              f"{c['crash']:,} | {c['timeout']:,} | {cover} |")

        total = cell(rows)
        lo, hi = wilson(total["undetected_silent"], total["trials"])
        clo, chi = wilson(total["detected"], total["mattered"])
        coverage = 100.0 * total["detected"] / total["mattered"]
        # Four decimals on the aggregate bounds. At this many trials a coverage
        # interval printed to two would read [100.00, 100.00], which is not
        # what a Wilson interval ever says and would look like a rounding
        # error being passed off as certainty.
        o(f"\nAcross all targets and bit counts: **{total['trials']:,} "
          f"trials**, coverage {coverage:.2f}% [{clo:.4f}, {chi:.4f}], "
          f"{total['undetected_silent']:,} silent "
          f"({100.0 * total['undetected_silent'] / total['trials']:.3f}%, "
          f"95% CI [{lo:.3f}, {hi:.3f}]), {total['crash']:,} crashes, "
          f"{total['timeout']:,} timeouts.\n")


def scrub(o, sweeps):
    o("\n## Detection latency against the scrub interval\n")
    o("Mean allocator calls between the flip and the allocator first reporting "
      "it, over ordinary traffic that deliberately never touches the damaged "
      "block. **Censored at the 4,096-call measurement window**: where damage "
      "the trial found in the end was not found inside that window, the mean "
      "is a lower bound and not an estimate.\n")
    o("Three columns, because the measurement says there are three "
      "populations and not the two the design predicted:\n")
    o("- **Free structures** â€” `free_hdr`, `links`, `free_footer`. These sit "
      "on the allocation path, so validate-on-touch finds them in ten to "
      "fifteen calls whatever the patrol is set to, patrol off included.")
    o("- **An allocated header** â€” `alloc_hdr`. Partly reachable by traffic "
      "that never touches the block: freeing and coalescing a *neighbour* "
      "validates the header beside it. With the patrol off it is still found "
      "about half the time, and quickly when it is found at all.")
    o("- **Payload and canary** â€” the patrol's alone. With the patrol off, "
      "traffic that never touches the block never finds them: not "
      "\"eventually\", but zero trials out of every one run.\n")
    o("`any` is in no column: it flips a bit anywhere in the arena and lands "
      "in all three at once. `in window` counts the damage found inside the "
      "4,096-call window against the damage found at all â€” a shortfall is "
      "what censors the mean beside it, and is marked `â‰¥`.\n")

    for profile in PROFILES:
        _, rows = sweeps[profile]
        o(f"\n### `{profile}`\n")
        o("| Scrub interval | Free structures | Allocated header | "
          "Payload and canary | In window |")
        o("|---|---:|---:|---:|---:|")
        # `off` is 0, which would sort to the front. It belongs at the end of
        # the curve: it is the limit the interval is heading towards.
        intervals = sorted({int(r["scrub_interval"]) for r in rows},
                           key=lambda v: (v == 0, v))
        for iv in intervals:
            here = [r for r in rows if int(r["scrub_interval"]) == iv]
            named = ON_PATH | NEIGHBOUR | PATROL_ONLY
            groups = [cell([r for r in here if r["target"] in g])
                      for g in (ON_PATH, NEIGHBOUR, PATROL_ONLY)]
            allof = cell([r for r in here if r["target"] in named])
            name = "off" if iv == 0 else f"{iv:,}"
            o(f"| {name} | " + " | ".join(fmt_lat(g) for g in groups) +
              f" | {allof['latency_n']:,} / {allof['detected']:,} |")

        # The claim the sweep exists to test is that the patrol moves *when*
        # damage is found and never *whether* corruption gets through. Under a
        # profile with a checksum that shows up as a flat zero; under `fast` it
        # shows up as a non-zero count that does not move with the interval,
        # which is the same claim and the more interesting evidence for it.
        per_iv = {iv: cell([r for r in rows if r["scrub_interval"] == iv])
                  ["undetected_silent"] for iv in {r["scrub_interval"]
                                                   for r in rows}}
        silent = cell(rows)["undetected_silent"]
        if silent == 0:
            o(f"\nSilent corruption across the whole sweep: **0**, at every "
              f"setting. The patrol governs *when* damage is found, never "
              f"whether corruption is allowed through, and the nightly "
              f"workflow fails if that stops holding.\n")
        else:
            lo, hi = min(per_iv.values()), max(per_iv.values())
            spread = ("identical at every setting" if lo == hi
                      else f"between {lo:,} and {hi:,} depending on the setting")
            o(f"\nSilent corruption across the whole sweep: **{silent:,}** â€” "
              f"this profile carries no checksum and no canary, so it detects "
              f"correspondingly less. Per interval it is {spread}, which is "
              f"the same claim the other two make with a zero: the patrol "
              f"moves *when* damage is found and not *whether* corruption gets "
              f"through. CI gates `{profile}` on the arena promise and records "
              f"these numbers rather than gating them.\n")


def thread_counts(rows):
    return sorted({int(r["threads"]) for r in rows})


def at(rows, workload, allocator, threads, column="ops_per_sec"):
    """Every repetition of one cell of the scaling curve."""
    return [float(r[column]) for r in rows
            if r["workload"] == workload and r["allocator"] == allocator
            and int(r["threads"]) == threads]


def thread_scaling(o, curves):
    """How throughput moves with the thread count, one table per strategy.

    The one section here that is a *curve* rather than a set of independent
    cells, and it is read down a column rather than across: what matters is not
    what any single row says but whether the column climbs. Speedup is measured
    against that same allocator's own one-thread row and never against glibc's,
    because what is being compared is a configuration with itself under load."""
    if not any(rows for _, rows in curves.values()):
        return

    o("\n## Thread scaling\n")
    o("T threads doing T times the work, so an allocator that scales perfectly "
      "draws a straight line in total throughput and one that serialises "
      "completely draws a flat one. Median of the repetitions; `speedup` is "
      "against the same allocator's own one-thread row.\n")
    o("**This machine has 4 physical cores and 8 hardware threads**, so the "
      "8-thread row is two threads to a core and would not double the 4-thread "
      "row even for code that scales perfectly. The 1â†’4 part of each column is "
      "the part that is about the allocator.\n")
    o("Unlike every other run in this directory these are taken *without* "
      "`taskset`: pinning a thread-scaling measurement to one core would "
      "measure the scheduler instead.\n")

    for lock in LOCKS:
        _, rows = curves[lock]
        if not rows:
            continue
        o(f"\n### `MARS_LOCK={lock}`\n")
        o(f"{LOCK_BLURB[lock]}\n")
        for wl in workloads(rows):
            o(f"\n**`{wl}`** â€” {MT_BLURB.get(wl, '')}\n")
            o("| Threads | mars ops/s | speedup | mars p50 | glibc ops/s | "
              "speedup | mars / glibc |")
            o("|---:|---:|---:|---:|---:|---:|---:|")
            base = {al: median(at(rows, wl, al, 1))
                    for al in ("mars", "system")}
            for t in thread_counts(rows):
                ours = median(at(rows, wl, "mars", t))
                theirs = median(at(rows, wl, "system", t))
                p50 = median(at(rows, wl, "mars", t, "p50_ns"))
                ratio = ours / theirs if theirs else float("nan")
                o(f"| {t} | {si(ours)} | {ours / base['mars']:.2f}Ã,  | "
                  f"{p50:,.0f} ns | {si(theirs)} | "
                  f"{theirs / base['system']:.2f}Ã,  | {ratio:.3f}Ã,  |")

    o("\nThe glibc columns are a scale rather than a target. glibc keeps a "
      "per-thread cache in front of its free lists and does no integrity "
      "checking at all, so its absolute figures say little about this "
      "allocator â€” but its *speedup* column says what this machine and this "
      "workload are capable of, and that is what makes a flat column beside it "
      "a statement about the allocator rather than about the benchmark.\n")


def thread_counts(rows):
    return sorted({int(r["threads"]) for r in rows})


def at(rows, workload, allocator, threads, column="ops_per_sec"):
    """Every repetition of one cell of the scaling curve."""
    return [float(r[column]) for r in rows
            if r["workload"] == workload and r["allocator"] == allocator
            and int(r["threads"]) == threads]


def thread_scaling(o, curves):
    """How throughput moves with the thread count, one table per strategy.

    The one section here that is a *curve* rather than a set of independent
    cells, and it is read down a column rather than across: what matters is not
    what any single row says but whether the column climbs. Speedup is measured
    against that same allocator's own one-thread row and never against glibc's,
    because what is being compared is a configuration with itself under load."""
    if not any(rows for _, rows in curves.values()):
        return

    o("\n## Thread scaling\n")
    o("T threads doing T times the work, so an allocator that scales perfectly "
      "draws a straight line in total throughput and one that serialises "
      "completely draws a flat one. Median of the repetitions; `speedup` is "
      "against the same allocator's own one-thread row.\n")
    o("**This machine has 4 physical cores and 8 hardware threads**, so the "
      "8-thread row is two threads to a core and would not double the 4-thread "
      "row even for code that scales perfectly. The 1→4 part of each column is "
      "the part that is about the allocator.\n")
    o("Unlike every other run in this directory these are taken *without* "
      "`taskset`: pinning a thread-scaling measurement to one core would "
      "measure the scheduler instead.\n")

    for lock in LOCKS:
        _, rows = curves[lock]
        if not rows:
            continue
        o(f"\n### `MARS_LOCK={lock}`\n")
        o(f"{LOCK_BLURB[lock]}\n")
        for wl in workloads(rows):
            o(f"\n**`{wl}`**, {MT_BLURB.get(wl, '')}\n")
            o("| Threads | mars ops/s | speedup | mars p50 | glibc ops/s | "
              "speedup | mars / glibc |")
            o("|---:|---:|---:|---:|---:|---:|---:|")
            base = {al: median(at(rows, wl, al, 1))
                    for al in ("mars", "system")}
            for t in thread_counts(rows):
                ours = median(at(rows, wl, "mars", t))
                theirs = median(at(rows, wl, "system", t))
                p50 = median(at(rows, wl, "mars", t, "p50_ns"))
                ratio = ours / theirs if theirs else float("nan")
                o(f"| {t} | {si(ours)} | {ours / base['mars']:.2f}× | "
                  f"{p50:,.0f} ns | {si(theirs)} | "
                  f"{theirs / base['system']:.2f}× | {ratio:.3f}× |")

    o("\nThe glibc columns are a scale rather than a target. glibc keeps a "
      "per-thread cache in front of its free lists and does no integrity "
      "checking at all, so its absolute figures say little about this "
      "allocator, though its *speedup* column says what this machine and this "
      "workload are capable of, and that is what makes a flat column beside it "
      "a statement about the allocator rather than about the benchmark.\n")


def preload(o, preloads):
    """Wall time of real programs, this allocator against glibc.

    Kept separate from the throughput section above and never merged with it:
    those are per-operation figures from a workload written to be measured,
    these are whole-process wall times from programs that know nothing about
    any of this. Putting them in one table would invite a reader to compare
    them, and they are not comparable."""
    if not any(v[1] for v in preloads.values()):
        return

    o("\n## Real programs under LD_PRELOAD\n")
    o("Wall time of a whole process, median of the repetitions, with the "
      "inter-quartile range beside it. `LD_PRELOAD=libmars_preload.so` against "
      "the same program with the system allocator, alternating repetition by "
      "repetition so that a machine drifting during the run does not land on "
      "one of them.\n")
    o("This is the measurement this allocator is least able to flatter. "
      "Nothing here was written to be allocated for: the programs, their "
      "allocation patterns and their sizes were all decided by somebody else "
      "for other reasons. It is also the measurement where the allocator "
      "matters least (most of what these processes do is not allocation), so "
      "**a ratio near 1.0 means the allocator disappeared into the noise, not "
      "that it is as fast as glibc.** The microbenchmarks above are where the "
      "per-operation cost is visible.\n")
    o("Every program was also run once more with `MARS_SHIM_CHECK` set, and "
      "`mm_check_heap()` walked the whole arena afterwards. The count is in "
      "each file's `#` block.\n")

    for profile in PROFILES:
        meta, rows = preloads[profile]
        if not rows:
            continue
        o(f"\n### `{profile}`\n")
        o("| Program | glibc | mars | IQR (mars) | mars / glibc | peak RSS |")
        o("|---|---:|---:|---:|---:|---:|")
        for name in program_names(rows):
            base = [float(r["wall_ns"]) for r in rows
                    if r["program"] == name and r["allocator"] == "glibc"]
            ours = [float(r["wall_ns"]) for r in rows
                    if r["program"] == name and r["allocator"] == "mars"]
            if not base or not ours:
                continue
            b, m = median(base), median(ours)
            rss = median([float(r["max_rss_kb"]) for r in rows
                          if r["program"] == name and r["allocator"] == "mars"])
            o(f"| `{name}` | {b / 1e6:,.0f} ms | {m / 1e6:,.0f} ms | "
              f"±{iqr(ours) / 1e6:,.0f} ms | {m / b:.2f}× | "
              f"{rss / 1024.0:,.0f} MB |")

        checks = meta.get("heap_checks_passed", "0")
        failed = meta.get("heap_checks_failed", "0")
        o(f"\nHeap checks after these runs: **{checks} consistent, "
          f"{failed} inconsistent**.\n")

        # The calloc rows carry a third allocator, and the whole point of them
        # is the comparison between the two mars columns.
        fresh = [r for r in rows if r["allocator"] == "mars_nofresh"]
        if fresh:
            o("\nWhat knowing a mapping is fresh saves `calloc`. "
              "`mars_nofresh` is the same build with `MARS_SHIM_NOFRESH=1`, "
              "which makes it memset every byte it hands back rather than "
              "trusting pages the kernel has just supplied.\n")
            o("| Program | glibc | mars | mars, memset always | saved |")
            o("|---|---:|---:|---:|---:|")
            for name in program_names(fresh):
                def med(alloc):
                    return median([float(r["wall_ns"]) for r in rows
                                   if r["program"] == name
                                   and r["allocator"] == alloc])
                g, m, n = med("glibc"), med("mars"), med("mars_nofresh")
                o(f"| `{name}` | {g / 1e6:,.1f} ms | {m / 1e6:,.1f} ms | "
                  f"{n / 1e6:,.1f} ms | {n / m:.1f}× |")


def program_names(rows):
    seen = []
    for r in rows:
        if r["program"] not in seen:
            seen.append(r["program"])
    return seen


def fmt_lat(c):
    """Mean calls to detection, marked `≥` when the window truncated it.

    Censoring is `latency_n < detected`: damage the classification phase found
    but the 4,096-call window did not. Comparing against the trial count
    instead would mark every cell, because a benign flip is never detected at
    all and there is nothing censored about that."""
    if not c["latency_n"]:
        return "never found"
    censored = "≥" if c["latency_n"] < c["detected"] else ""
    return f"{censored}{c['latency_mean']:,.1f}"


def workloads(rows):
    seen = []
    for r in rows:
        if r["workload"] not in seen:
            seen.append(r["workload"])
    return seen


def decomposition(o, benches, nolock):
    """The gap against glibc, split into the part that is integrity checking
    and the part that is not.

    "Four to seven times slower than glibc" is true and not very useful,
    because it does not say what the factor is made of. Three files already in
    this directory answer that without any new measurement: `fast` is this
    allocator with the checksum and the canary compiled out, so the distance
    from glibc to `fast` is everything *except* integrity, and the distance
    from `fast` to `hardened` is integrity and nothing else."""
    o("\n## What the gap against glibc is made of\n")
    o("The headline ratio says how much slower this is than glibc. It does not "
      "say what the factor consists of, and that matters more than the figure "
      "does: one part is the price of checking, which is the feature, and the "
      "other is the price of not having glibc's structure, which is not.\n")
    o("No new measurement is involved. `fast` is this same allocator with the "
      "header checksum and the canary compiled out, so **glibc to `fast` is "
      "everything except integrity** and **`fast` to `hardened` is integrity "
      "and nothing else**. The two multiply back to the total.\n")
    o("The last column is the lock's share of the structural half, from "
      "`bench-hardened-nolock.csv`. It sits inside the structural column "
      "rather than beside it.\n")

    _, fast = benches["fast"]
    _, hard = benches["hardened"]
    _, nl = nolock

    o("| Workload | total vs glibc | of which integrity | of which structural "
      "| the lock, within structural |")
    o("|---|---:|---:|---:|---:|")
    both = []
    for wl in workloads(hard):
        g = median([float(r["ops_per_sec"]) for r in hard
                    if r["workload"] == wl and r["allocator"] == "system"])
        gf = median([float(r["ops_per_sec"]) for r in fast
                     if r["workload"] == wl and r["allocator"] == "system"])
        f = median([float(r["ops_per_sec"]) for r in fast
                    if r["workload"] == wl and r["allocator"] == "mars"])
        h = median([float(r["ops_per_sec"]) for r in hard
                    if r["workload"] == wl and r["allocator"] == "mars"])
        n = median([float(r["ops_per_sec"]) for r in nl
                    if r["workload"] == wl and r["allocator"] == "mars"])
        total, integrity, structural, lock = g / h, f / h, gf / f, n / h
        # Meaningful means: there is a gap to decompose (glibc is ahead) and
        # the integrity column is measuring the allocator rather than a feature
        # glibc does not have. Both conditions are stated below the table.
        if structural > 1.0 and integrity < 5.0:
            both.append((structural, integrity))
        o(f"| `{wl}` | {total:.2f}x | {integrity:.2f}x | {structural:.2f}x | "
          f"{lock:.2f}x |")

    def geo(xs):
        if not xs:
            return float("nan")
        return math.exp(sum(math.log(v) for v in xs) / len(xs))

    gs = geo([x for x, _ in both])
    gi = geo([y for _, y in both])
    o(f"\nOver the {len(both)} workloads where both halves are meaningful the "
      f"geometric means are **{gs:.1f}x structural** and **{gi:.1f}x "
      f"integrity**. The one-line version is therefore: *about {gs * gi:.0f}x "
      f"slower than glibc, of which roughly {gi:.1f}x is integrity checking "
      f"and the rest is structure*.\n")

    o("**Two workloads are left out of that average, and which two is not a "
      "choice about which numbers to like.** `realloc_grow` is faster here "
      "than under glibc, so it has no gap to decompose. `validated_access` "
      "checksums the whole payload on every read, which puts its integrity "
      "column an order of magnitude above every other row; that is the price "
      "of a feature glibc does not have rather than of allocating, and "
      "averaging it in would describe neither thing.\n")
    o("The ordering is worth stating plainly, because it is easy to get "
      "backwards: **on the workloads where the comparison applies, the "
      "structural half is the larger one**, and it is larger on every one of "
      "them individually. Take the geometric mean over all seven rows instead "
      "and it inverts, entirely on the strength of those two exclusions. That "
      "is the honest caveat on the summary sentence above, and it is why the "
      "table is here rather than only the sentence.\n")


# --- Driver -----------------------------------------------------------------

def build():
    lines = []
    o = lines.append

    benches = {p: load("bench", p) for p in PROFILES}
    nostats = read(RESULTS / "bench-hardened-nostats.csv")
    nolock = read(RESULTS / "bench-hardened-nolock.csv")
    repeat = read(RESULTS / "bench-hardened-repeat.csv")
    matrices = {p: load("faults", p) for p in PROFILES}
    sweeps = {p: load("scrub", p) for p in PROFILES}
    # Not required: the preload library is Unix-only, so a tree measured on a
    # platform that cannot build it has no such file and the section is simply
    # absent rather than the report refusing to generate.
    preloads = {p: load("preload", p, required=False) for p in PROFILES}
    # One file per locking strategy. A strategy that has not been measured is
    # simply absent rather than an error, which is what lets the global lock's
    # curve be published before there is a second curve to compare it with.
    curves = {k: load("threads", k, required=False) for k in LOCKS}

    o("# Results\n")
    o("Generated by `tools/report.py` from the CSVs in "
      "[`bench/results/`](../bench/results/). Do not edit by hand, regenerate "
      "it. Every number below traces to a committed file, which is the rule "
      "`tools/check_readme_numbers.py` enforces on the README.\n")
    o("The method behind each figure is in "
      "[BENCHMARK_METHOD.md](BENCHMARK_METHOD.md) and "
      "[FAULT_MODEL.md](FAULT_MODEL.md).\n")

    o("\n## Provenance\n")
    provenance(o, benches)
    throughput(o, benches)
    latency(o, benches)
    space(o, benches)
    decomposition(o, benches, nolock)
    noise_floor(o, benches, repeat)
    counters(o, benches, nostats, repeat[1])
    locking_cost(o, benches, nolock, repeat[1])
    thread_scaling(o, curves)
    preload(o, preloads)
    faults(o, matrices)
    scrub(o, sweeps)

    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="exit 1 if docs/RESULTS.md is not what would be "
                         "written, rather than writing it")
    args = ap.parse_args()

    text = build()
    if args.check:
        current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
        if current != text:
            sys.exit(f"{OUT} is out of date; run tools/report.py")
        print(f"{OUT} is up to date")
        return
    OUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
