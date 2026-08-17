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
  faults-<profile>.csv           the full target x bits matrix
  scrub-<profile>.csv            the same targets swept over scrub intervals

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
      "fault-injection counts below do not have this problem — they come from "
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


def counters(o, benches, nostats):
    o("\n## What the counters cost\n")
    o("`MARS_STATS` is on by default and is what supplies the utilisation and "
      "overhead columns above. Both builds are Release, hardened, same "
      "machine, same seeds; the only difference is whether the counters are "
      "compiled in.\n")
    _, on = benches["hardened"]
    _, off = nostats
    o("| Workload | counters on | counters off | cost |")
    o("|---|---:|---:|---:|")
    for wl in workloads(on):
        a = median([float(r["ops_per_sec"]) for r in on
                    if r["workload"] == wl and r["allocator"] == "mars"])
        b = median([float(r["ops_per_sec"]) for r in off
                    if r["workload"] == wl and r["allocator"] == "mars"])
        o(f"| `{wl}` | {si(a)} | {si(b)} | {100.0 * (b - a) / a:+.1f}% |")
    o("\nRead this against the spread in the throughput table above: where the "
      "difference is smaller than the inter-quartile range, the honest reading "
      "is that the counters cost less than this machine can measure.\n")


def faults(o, matrices):
    o("\n## Fault injection\n")
    m0 = matrices["hardened"][0]
    o(f"{int(m0.get('trials_per_cell', 0)):,} trials per cell, "
      f"{len(TARGETS)} targets × {{1, 2, 4, 8}} bits × {len(PROFILES)} "
      f"profiles. Each trial forks a child under an alarm, flips *k* bits in "
      f"the chosen structure, and classifies what the allocator then does "
      f"against a shadow model of what every live payload is supposed to "
      f"contain. Base seed `{m0.get('base_seed', '?')}`, arena "
      f"{int(m0.get('arena_bytes', 0)):,} bytes, patrol at its 1024-call "
      f"default, at commit `{m0.get('git_sha', '?')}`.\n")
    o("**Detection coverage is `detected / (detected + silent)`** — of the "
      "flips that actually mattered, how many were caught. Benign flips landed "
      "in slack, padding or free space and are excluded, because there was "
      "nothing there to catch. Intervals are 95% Wilson score.\n")

    for profile in PROFILES:
        meta, rows = matrices[profile]
        o(f"\n### `{profile}` — {meta.get('metadata_bytes', '?')} B of "
          f"metadata per allocation\n")
        o("| Target | Trials | Benign | Detected | Silent | Crash | Timeout | "
          "Coverage (95% CI) |")
        o("|---|---:|---:|---:|---:|---:|---:|---:|")
        for target in TARGETS:
            got = [r for r in rows if r["target"] == target]
            if not got:
                o(f"| `{target}` | – | – | – | – | – | – | "
                  f"no such structure under this profile |")
                continue
            c = cell(got)
            lo, hi = wilson(c["detected"], c["mattered"])
            pct = (100.0 * c["detected"] / c["mattered"]
                   if c["mattered"] else float("nan"))
            cover = ("n/a — nothing mattered" if not c["mattered"]
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
    o("- **Free structures** — `free_hdr`, `links`, `free_footer`. These sit "
      "on the allocation path, so validate-on-touch finds them in ten to "
      "fifteen calls whatever the patrol is set to, patrol off included.")
    o("- **An allocated header** — `alloc_hdr`. Partly reachable by traffic "
      "that never touches the block: freeing and coalescing a *neighbour* "
      "validates the header beside it. With the patrol off it is still found "
      "about half the time, and quickly when it is found at all.")
    o("- **Payload and canary** — the patrol's alone. With the patrol off, "
      "traffic that never touches the block never finds them: not "
      "\"eventually\", but zero trials out of every one run.\n")
    o("`any` is in no column: it flips a bit anywhere in the arena and lands "
      "in all three at once. `in window` counts the damage found inside the "
      "4,096-call window against the damage found at all — a shortfall is "
      "what censors the mean beside it, and is marked `≥`.\n")

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
            o(f"\nSilent corruption across the whole sweep: **{silent:,}** — "
              f"this profile carries no checksum and no canary, so it detects "
              f"correspondingly less. Per interval it is {spread}, which is "
              f"the same claim the other two make with a zero: the patrol "
              f"moves *when* damage is found and not *whether* corruption gets "
              f"through. CI gates `{profile}` on the arena promise and records "
              f"these numbers rather than gating them.\n")


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


# --- Driver -----------------------------------------------------------------

def build():
    lines = []
    o = lines.append

    benches = {p: load("bench", p) for p in PROFILES}
    nostats = read(RESULTS / "bench-hardened-nostats.csv")
    matrices = {p: load("faults", p) for p in PROFILES}
    sweeps = {p: load("scrub", p) for p in PROFILES}

    o("# Results\n")
    o("Generated by `tools/report.py` from the CSVs in "
      "[`bench/results/`](../bench/results/). Do not edit by hand — regenerate "
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
    counters(o, benches, nostats)
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
