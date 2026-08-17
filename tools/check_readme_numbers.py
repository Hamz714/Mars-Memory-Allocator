#!/usr/bin/env python3
"""Fail if the README makes a numeric claim nothing in the repository backs.

The rule being enforced is: **no number reaches the README until it exists in a
committed CSV.** A rule nobody checks is a rule that decays, and a results table
that has drifted from the measurements behind it is worse than no table at all
-- it is confidently wrong.

Every number in the README's prose and tables must resolve to one of three
sources, and the script says which one it used:

  csv     a value in bench/results/*.csv, or an aggregate of them -- medians,
          inter-quartile ranges, mars/glibc ratios, detection coverage, Wilson
          bounds, totals. Computed here the same way tools/report.py computes
          them for docs/RESULTS.md.
  source  a numeric #define in the headers: block sizes, bin counts, the scrub
          defaults. A structural constant is not measured, but it is not
          asserted either -- it has to exist in the code.
  stated  a short, explicit list in this file. Anything here is a definition or
          an identifier rather than a measurement (the 95% of a 95% interval,
          the 2^-32 of a CRC, a toolchain version) and carries the reason it is
          exempt. Keeping the list short and visible is what stops it becoming
          the loophole that makes the gate meaningless.

Numbers inside fenced code blocks, inline `code` spans and link targets are not
claims -- they are commands, flags and paths -- and are skipped.

  python3 tools/check_readme_numbers.py            # the CI gate
  python3 tools/check_readme_numbers.py --explain  # show where each one came from
"""

import argparse
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import report  # noqa: E402  -- same directory, and the aggregates must agree

REPO = pathlib.Path(__file__).resolve().parent.parent
README = REPO / "README.md"
HEADERS = ["src/mm_layout.h", "src/mm_freelist.h", "src/mm_internal.h",
           "include/mars/allocator.h"]

# Definitions rather than measurements. Each one is here because there is no
# file it could come from, and each carries why.
STATED = {
    95: "the confidence level of a Wilson score interval",
    32: "the width of CRC32C, and the 2^-32 that follows from it",
    3: "the three integrity profiles, and the 1-, 2- and 3-bit errors a "
       "32-bit CRC detects outright",
    2: "the two of 1-, 2- and 3-bit errors, and the two detection tiers",
    1: "the one of 1-, 2- and 3-bit errors",
    0: "zero, as a claim about counts that are zero",
    11: "repetitions in the pinned run, a parameter of the run rather than "
        "an output of it",
    # Toolchain identifiers. The kernel and compiler each committed run was
    # actually taken with are in that file's own `#` header; these name the
    # environment somebody reproducing it should stand up.
    22.04: "Ubuntu 22.04, the environment the committed runs were taken in",
    3.22: "the CMake version that environment carries",
}

NUMBER = re.compile(r"\d[\d,]*(?:\.\d+)?")


# --- What the README is allowed to say --------------------------------------

def csv_candidates():
    """Every value a README number could legitimately be quoting, and the
    aggregates docs/RESULTS.md derives from them."""
    out = set()

    def add(x):
        try:
            f = float(x)
        except (TypeError, ValueError):
            return
        out.add(abs(f))

    benches = {p: report.load("bench", p) for p in report.PROFILES}
    benches["hardened-nostats"] = report.read(
        report.RESULTS / "bench-hardened-nostats.csv")
    matrices = {p: report.load("faults", p) for p in report.PROFILES}
    sweeps = {p: report.load("scrub", p) for p in report.PROFILES}

    # Raw cells, from every file in the directory including the earlier
    # reference runs -- a number lifted straight out of a CSV is traceable
    # whichever file it came from.
    for path in sorted(report.RESULTS.glob("*.csv")):
        meta, rows = report.read(path)
        for v in meta.values():
            add(v)
        for row in rows:
            for v in row.values():
                add(v)

    # Throughput, latency and space, aggregated as RESULTS.md aggregates them.
    for _, rows in benches.values():
        for wl in report.workloads(rows):
            for al in ("mars", "system"):
                got = [r for r in rows
                       if r["workload"] == wl and r["allocator"] == al]
                for col in ("ops_per_sec", "p50_ns", "p99_ns", "p999_ns",
                            "mean_ns", "util_pct", "meta_bytes_per_alloc"):
                    vals = [float(r[col]) for r in got]
                    add(report.median(vals))
                    add(report.iqr(vals))
                    add(report.iqr(vals) / 2)
            mars = [float(r["ops_per_sec"]) for r in rows
                    if r["workload"] == wl and r["allocator"] == "mars"]
            sysl = [float(r["ops_per_sec"]) for r in rows
                    if r["workload"] == wl and r["allocator"] == "system"]
            if report.median(sysl):
                add(report.median(mars) / report.median(sysl))
                add(report.median(sysl) / report.median(mars))

        # The counter cost, as a percentage either way round.
        on = benches["hardened"][1]
        off = benches["hardened-nostats"][1]
        for wl in report.workloads(on):
            a = report.median([float(r["ops_per_sec"]) for r in on
                               if r["workload"] == wl and r["allocator"] == "mars"])
            b = report.median([float(r["ops_per_sec"]) for r in off
                               if r["workload"] == wl and r["allocator"] == "mars"])
            if a:
                add(100.0 * (b - a) / a)

    # Fault injection: per target, per profile, and the totals across all of
    # them -- which is the headline the README is most likely to quote.
    every = []
    for group in (matrices, sweeps):
        for _, rows in group.values():
            every += rows
            for target in report.TARGETS:
                got = [r for r in rows if r["target"] == target]
                if got:
                    add_cell(add, report.cell(got))
            add_cell(add, report.cell(rows))
            for iv in {r["scrub_interval"] for r in rows}:
                here = [r for r in rows if r["scrub_interval"] == iv]
                add_cell(add, report.cell(here))
                for g in (report.ON_PATH, report.NEIGHBOUR,
                          report.PATROL_ONLY):
                    add_cell(add, report.cell(
                        [r for r in here if r["target"] in g]))
    add_cell(add, report.cell(every))
    # Trial counts summed across profiles: "840,000 trials" is a claim about
    # the whole matrix, not about any one file.
    for group in (matrices, sweeps):
        add(sum(int(r["trials"]) for _, rows in group.values() for r in rows))

    # Counts the CSVs themselves establish: how many workloads are frozen, how
    # many targets exist, how many profiles were run.
    add(len(report.workloads(benches["hardened"][1])))
    add(len(report.PROFILES))
    add(len({r["target"] for _, rows in matrices.values() for r in rows}))
    add(len({r["bits"] for _, rows in matrices.values() for r in rows}))
    add(len({r["scrub_interval"] for _, rows in sweeps.values() for r in rows}))
    return out


def add_cell(add, c):
    for v in c.values():
        add(v)
    if c["mattered"]:
        pct = 100.0 * c["detected"] / c["mattered"]
        add(pct)
        add(100.0 - pct)
        for b in report.wilson(c["detected"], c["mattered"]):
            add(b)
    if c["trials"]:
        add(100.0 * c["undetected_silent"] / c["trials"])
        add(100.0 * c["crash"] / c["trials"])
        for b in report.wilson(c["undetected_silent"], c["trials"]):
            add(b)


DEFINE = re.compile(r"^#\s*define\s+(MM_[A-Z0-9_]+)\s+\(?\(?[a-z_]*\)?"
                    r"\s*(\d+)\)?\s*(?://.*)?$")


def source_candidates():
    """Numeric #defines in the headers. A structural constant is not measured,
    but it does have to exist in the code rather than only in the README."""
    out = {}
    for rel in HEADERS:
        for line in (REPO / rel).read_text(encoding="utf-8").splitlines():
            m = DEFINE.match(line.strip())
            if m:
                out.setdefault(float(m.group(2)), f"{m.group(1)} in {rel}")
    return out


# --- Reading the README -----------------------------------------------------

def claims(text):
    """Numeric tokens the README asserts, with the line they sit on. Code
    spans, fenced blocks and link targets are commands and paths, not claims."""
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    out = []
    for lineno, line in enumerate(text.splitlines(), 1):
        line = re.sub(r"`[^`]*`", "", line)
        line = re.sub(r"\]\([^)]*\)", "]", line)
        for m in NUMBER.finditer(line):
            token = m.group(0).rstrip(".,")
            try:
                value = float(token.replace(",", ""))
            except ValueError:
                continue
            decimals = len(token.split(".")[1]) if "." in token else 0
            out.append((lineno, token, value, decimals, line.strip()))
    return out


def resolve(value, decimals, csv_vals, src_vals):
    if value in STATED:
        return "stated", STATED[value]
    if value in src_vals:
        return "source", src_vals[value]
    for c in csv_vals:
        if round(c, decimals) == value:
            return "csv", f"matches {c:.6g} in bench/results/"
    return None, None


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--explain", action="store_true",
                    help="print where every number resolved, not only failures")
    args = ap.parse_args()

    text = README.read_text(encoding="utf-8")
    bad = []

    # A placeholder that survived into the README is the failure this gate was
    # written for first: it means a table was laid out and never filled in.
    for lineno, line in enumerate(text.splitlines(), 1):
        if "__" in line:
            bad.append((lineno, "__", "placeholder left in the README"))

    csv_vals = csv_candidates()
    src_vals = source_candidates()

    for lineno, token, value, decimals, line in claims(text):
        kind, why = resolve(value, decimals, csv_vals, src_vals)
        if kind is None:
            bad.append((lineno, token, f"no CSV, header or stated source: {line}"))
        elif args.explain:
            print(f"  README.md:{lineno:>4}  {token:>14}  {kind:<6}  {why}")

    if bad:
        print(f"{len(bad)} unbacked claim(s) in README.md:\n", file=sys.stderr)
        for lineno, token, why in bad:
            print(f"  README.md:{lineno}: {token} -- {why}", file=sys.stderr)
        print("\nEvery number in the README must come from a committed CSV, a "
              "#define, or the STATED list in this script.", file=sys.stderr)
        sys.exit(1)

    print(f"README.md: {len(claims(text))} numeric claims, all backed")


if __name__ == "__main__":
    main()
