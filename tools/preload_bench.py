#!/usr/bin/env python3
"""Time real programs against glibc's allocator and against this one.

Every other measurement in this repository runs a workload that was written to
be measured. This one does not: it runs `git status`, a Python interpreter and
a compiler, and times them. That is a weaker signal per run -- most of what
those programs do has nothing to do with allocation -- and a much stronger
claim, because nobody chose the allocation pattern to suit the allocator.

  python3 tools/preload_bench.py --preload build/gcc-release/bin/libmars_preload.so \\
      --reps 7 --out bench/results/preload-hardened.csv

Output is the same shape as the other files in bench/results/: a `#` block
recording where the run was taken, then one row per repetition, unaggregated,
so that the median and the spread can be recomputed and argued with.

**Wall time of a whole process is a noisy instrument.** Process start-up,
dynamic linking, page cache and the scheduler are all in it, and on a laptop
under WSL2 they move by more than the allocator does. That is why the harness
alternates allocators repetition by repetition rather than running all of one
and then all of the other, why it warms the page cache first, and why
docs/RESULTS.md quotes the median with its inter-quartile range rather than a
single figure.
"""

import argparse
import datetime
import os
import pathlib
import platform
import shutil
import subprocess
import sys
import time

REPO = pathlib.Path(__file__).resolve().parent.parent


# --- The programs -----------------------------------------------------------
#
# Chosen for three reasons: they are present on any Linux with a toolchain, they
# allocate enough for the allocator to matter, and their allocation patterns
# are genuinely different from each other -- a directory walk, a content-
# addressed store, a bytecode interpreter's object churn, and a compiler.

def programs(repo, tmp, probe):
    out = [
        # /usr/include rather than /usr/lib: the same directory walk and the
        # same allocation pattern, over a tree small enough that a repetition
        # is seconds rather than a minute, and one every user can read -- so
        # the run exits 0 instead of 1 and a real failure would show.
        ("ls_recursive", ["ls", "-laR", "/usr/include"], {},
         ("glibc", "mars")),
        ("git_status", ["git", "-C", str(repo), "status", "--porcelain"], {},
         ("glibc", "mars")),
        ("git_log_stat",
         ["git", "-C", str(repo), "log", "--stat", "-n", "200"], {},
         ("glibc", "mars")),
        ("grep_recursive",
         ["grep", "-rc", "allocator", str(repo / "src"), str(repo / "docs"),
          str(repo / "tests")], {}, ("glibc", "mars")),
        ("python_sum",
         [sys.executable, "-c", "print(sum(range(10**6)))"], {},
         ("glibc", "mars")),
        ("python_dict",
         [sys.executable, "-c",
          "d={str(i):i*i for i in range(300000)}; print(len(d))"], {},
         ("glibc", "mars")),
        ("gcc_compile",
         ["gcc", "-O2", "-std=c11", "-c", str(repo / "src" / "mm_core.c"),
          "-I", str(repo / "include"), "-I", str(repo / "src"),
          "-o", str(tmp / "mm_core.o")], {}, ("glibc", "mars")),
    ]
    if probe is not None and probe.exists():
        # The fresh-mapping shortcut in calloc, priced against both the
        # allocator that does not take it and the one that has always taken it.
        out.append(("calloc_4mb_x200", [str(probe), "200", "4194304"], {},
                    ("glibc", "mars", "mars_nofresh")))
        out.append(("calloc_4kb_x200000", [str(probe), "200000", "4096"], {},
                    ("glibc", "mars", "mars_nofresh")))
    return out


# --- Running ----------------------------------------------------------------

def env_for(allocator, preload, check_log):
    env = dict(os.environ)
    for k in ("LD_PRELOAD", "MARS_SHIM_NOFRESH", "MARS_SHIM_CHECK",
              "MARS_SHIM_DEBUG", "MARS_SHIM_FLIP"):
        env.pop(k, None)
    if allocator == "glibc":
        return env
    env["LD_PRELOAD"] = str(preload)
    if check_log is not None:
        # Deliberately NOT set during the timed runs. mm_check_heap walks every
        # block in the arena, and that walk would land inside the wall time it
        # is meant to be reporting on -- a self-inflicted slowdown attributed to
        # the allocator. The consistency runs happen separately, afterwards.
        env["MARS_SHIM_CHECK"] = str(check_log)
    if allocator == "mars_nofresh":
        env["MARS_SHIM_NOFRESH"] = "1"
    return env


def run_once(argv, env):
    """Returns (wall_ns, max_rss_kb, exit_code). Output is discarded: this is a
    measurement of the program, not a test of what it prints."""
    with open(os.devnull, "wb") as null:
        start = time.perf_counter_ns()
        proc = subprocess.Popen(argv, stdout=null, stderr=null, env=env)
        _, status, usage = os.wait4(proc.pid, 0)
        wall = time.perf_counter_ns() - start
    # The child has already been reaped by wait4, so Popen must be told rather
    # than left to wait for a process that no longer exists.
    proc.returncode = os.waitstatus_to_exitcode(status)
    return wall, usage.ru_maxrss, proc.returncode


# --- Provenance -------------------------------------------------------------

def cpu_model():
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def compiler():
    try:
        first = subprocess.run(["gcc", "--version"], capture_output=True,
                               text=True).stdout.splitlines()[0]
        return " ".join(first.split()[:4])
    except Exception:
        return "unknown"


def provenance(args, count):
    now = datetime.datetime.now(datetime.timezone.utc)
    return [
        "# cpu=%s" % cpu_model(),
        "# kernel=%s %s" % (platform.system(), platform.release()),
        "# compiler=%s" % compiler(),
        "# date=%s" % now.strftime("%Y-%m-%dT%H:%M:%SZ"),
        "# git_sha=%s" % args.git_sha,
        "# profile=%s" % args.profile,
        "# preload=%s" % pathlib.Path(args.preload).name,
        "# reps=%d programs=%d" % (args.reps, count),
        "# note=wall time of whole processes; see tools/preload_bench.py",
    ]


# --- Main -------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preload", required=True,
                    help="path to libmars_preload.so")
    ap.add_argument("--out", required=True)
    ap.add_argument("--reps", type=int, default=7)
    ap.add_argument("--git-sha", default="unknown")
    ap.add_argument("--profile", default="hardened")
    ap.add_argument("--probe", default=None,
                    help="path to the calloc_probe binary")
    args = ap.parse_args()

    preload = pathlib.Path(args.preload).resolve()
    if not preload.exists():
        sys.exit("no such preload library: %s" % preload)

    tmp = pathlib.Path("/tmp/mars_preload_bench")
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    check_log = tmp / "heap_check.log"

    probe = pathlib.Path(args.probe).resolve() if args.probe else None
    progs = programs(REPO, tmp, probe)

    # Warm the page cache and the git object store, so that repetition 1 is not
    # measuring a cold start that no later repetition pays for.
    for name, argv, extra, _ in progs:
        e = env_for("glibc", preload, None)
        e.update(extra)
        run_once(argv, e)

    rows = []
    for rep in range(1, args.reps + 1):
        # Allocator alternates inside the repetition rather than outside it. A
        # machine that gets slower over the course of a run -- thermal, or
        # another process starting -- would otherwise put all of that on
        # whichever allocator went second.
        for name, argv, extra, allocators in progs:
            for allocator in allocators:
                e = env_for(allocator, preload, None)
                e.update(extra)
                wall, rss, code = run_once(argv, e)
                rows.append((name, allocator, rep, wall, rss, code))
                print("  %-20s %-13s rep %d  %8.1f ms  %7d KB  exit %d"
                      % (name, allocator, rep, wall / 1e6, rss, code),
                      file=sys.stderr)

    # Now, and only now, the same programs again with the heap check turned on.
    # Separate from the timed runs because the check is a full walk of the arena
    # and would otherwise be charged to the allocator's wall time; run at all
    # because a timing taken from a run that corrupted its heap is not a timing
    # anybody should quote.
    print("  -- verifying the heap after each program", file=sys.stderr)
    for name, argv, extra, allocators in progs:
        if "mars" not in allocators:
            continue
        e = env_for("mars", preload, check_log)
        e.update(extra)
        run_once(argv, e)

    consistent = 0
    inconsistent = 0
    if check_log.exists():
        text = check_log.read_text()
        consistent = text.count("heap consistent at exit")
        inconsistent = text.count("INCONSISTENT")

    lines = provenance(args, len(progs))
    lines.append("# heap_checks_passed=%d heap_checks_failed=%d"
                 % (consistent, inconsistent))
    lines.append("program,allocator,rep,wall_ns,max_rss_kb,exit_code")
    for r in rows:
        lines.append("%s,%s,%d,%d,%d,%d" % r)

    out = pathlib.Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print("wrote %s: %d rows" % (out, len(rows)), file=sys.stderr)

    if inconsistent:
        sys.exit("%d run(s) left an inconsistent heap; the timings above are "
                 "not usable" % inconsistent)
    if consistent == 0:
        sys.exit("no run reported a heap check; MARS_SHIM_CHECK did not take")


if __name__ == "__main__":
    main()
