#!/usr/bin/env python3
# Copyright 2026 Slick Quant
# SPDX-License-Identifier: MIT
"""A/B benchmark comparison between two builds on the same machine.

Runs every benchmark the two builds share, one benchmark at a time, alternating
base and head (and swapping which goes first each round) so that load drift
lands on both equally. Each variant keeps the best median `real_time` it reaches
across rounds; the verdict is the geometric mean of head/base over all shared
benchmarks, plus a looser per-benchmark limit. Anything over a limit is measured
again for more rounds before it is reported, so one noisy round cannot fail it.

    ab_compare.py --base build-base/benchmarks --head build/benchmarks

Exits 1 on a confirmed regression, 2 on a usage or run error.
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys

_TIME_SCALE = {"ns": 1.0, "us": 1e3, "ms": 1e6, "s": 1e9}


def find_exe(directory, name):
    for rel in (name, name + ".exe", os.path.join("Release", name + ".exe"),
                os.path.join("Release", name)):
        path = os.path.join(directory, rel)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    return None


def list_benchmarks(exe):
    out = subprocess.run([exe, "--benchmark_list_tests=true"], check=True, capture_output=True,
                         text=True).stdout
    return [line.strip() for line in out.splitlines() if line.strip()]


def median_ns(exe, name, args):
    """Median real_time of one benchmark, in nanoseconds."""
    cmd = [
        exe,
        "--benchmark_filter=^" + re.escape(name) + "$",
        "--benchmark_repetitions=%d" % args.reps,
        "--benchmark_min_time=%s" % args.min_time,
        "--benchmark_report_aggregates_only=true",
        "--benchmark_format=json",
    ]
    report = json.loads(subprocess.run(cmd, check=True, capture_output=True, text=True).stdout)
    for b in report["benchmarks"]:
        if b.get("aggregate_name") == "median":
            return b["real_time"] * _TIME_SCALE[b.get("time_unit", "ns")]
    raise RuntimeError("no median reported for %s by %s" % (name, exe))


def measure(cases, best, rounds, args):
    """Alternate base/head per benchmark for `rounds` rounds, keeping best medians."""
    for r in range(rounds):
        for key, base_exe, head_exe in cases:
            order = (("base", base_exe), ("head", head_exe))
            for side, exe in (order if r % 2 == 0 else order[::-1]):
                t = median_ns(exe, key[1], args)
                slot = best.setdefault(key, {})
                slot[side] = min(t, slot.get(side, math.inf))


def ratios(best):
    return {k: v["head"] / v["base"] for k, v in best.items()}


def geomean(values):
    return math.exp(sum(math.log(v) for v in values) / len(values))


def report(best, args, confirmed):
    r = ratios(best)
    g = geomean(r.values())
    lines = [
        "### Benchmark A/B: head vs base",
        "",
        "Best median `real_time` over %d+ rounds of %d repetitions, alternated per benchmark "
        "on one runner. Positive change is slower." % (args.rounds, args.reps),
        "",
        "| Benchmark | base (ns) | head (ns) | change |",
        "|---|---:|---:|---:|",
    ]
    for key in best:
        flag = " :warning:" if r[key] > 1 + args.max_regression else ""
        lines.append("| `%s/%s` | %.1f | %.1f | %+.1f%%%s |" %
                     (key[0], key[1], best[key]["base"], best[key]["head"],
                      (r[key] - 1) * 100, flag))
    lines += [
        "",
        "**Geometric mean: %+.2f%%** (limit +%.0f%%; per benchmark +%.0f%%)%s" %
        ((g - 1) * 100, args.max_geomean_regression * 100, args.max_regression * 100,
         " — confirmed with %d extra rounds" % args.confirm_rounds if confirmed else ""),
        "",
    ]
    text = "\n".join(lines)
    print(text)
    if args.summary:
        with open(args.summary, "a", encoding="utf-8") as f:
            f.write(text + "\n")


def failing(best, args):
    r = ratios(best)
    worst = [k for k, v in r.items() if v > 1 + args.max_regression]
    return geomean(r.values()) > 1 + args.max_geomean_regression, worst


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--base", required=True, help="benchmark build directory of the baseline")
    p.add_argument("--head", required=True, help="benchmark build directory of the change")
    p.add_argument("--bench", action="append", help="benchmark executable (repeatable)")
    p.add_argument("--rounds", type=int, default=3)
    p.add_argument("--confirm-rounds", type=int, default=5)
    p.add_argument("--reps", type=int, default=5)
    p.add_argument("--min-time", default="0.05s")
    p.add_argument("--max-geomean-regression", type=float, default=0.05)
    p.add_argument("--max-regression", type=float, default=0.25)
    p.add_argument("--pin", type=int, help="CPU to pin every run to (Linux)")
    p.add_argument("--summary", default=os.environ.get("GITHUB_STEP_SUMMARY"),
                   help="markdown file to append the report to")
    args = p.parse_args()

    if args.pin is not None and hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, {args.pin})  # inherited by every benchmark run

    cases = []
    for bench in args.bench or ["bench_quote", "bench_events"]:
        base_exe, head_exe = find_exe(args.base, bench), find_exe(args.head, bench)
        if head_exe is None:
            print("error: %s not found under %s" % (bench, args.head), file=sys.stderr)
            return 2
        if base_exe is None:
            print("note: %s not in the baseline; skipped" % bench)
            continue
        base_names = set(list_benchmarks(base_exe))
        for name in list_benchmarks(head_exe):
            if name in base_names:
                cases.append(((bench, name), base_exe, head_exe))
            else:
                print("note: %s/%s is new; no baseline to compare" % (bench, name))
    if not cases:
        print("No benchmarks shared with the baseline; nothing to compare.")
        return 0

    best = {}
    measure(cases, best, args.rounds, args)
    geo_bad, worst = failing(best, args)
    confirmed = geo_bad or bool(worst)
    if confirmed:
        # Re-measure everything that got slower, so the verdict rests on more rounds.
        slower = set(k for k, v in ratios(best).items() if v > 1)
        measure([c for c in cases if c[0] in slower], best, args.confirm_rounds, args)
        geo_bad, worst = failing(best, args)

    report(best, args, confirmed)
    if geo_bad or worst:
        print("Benchmark regression: geomean %s, %d benchmark(s) over the per-benchmark limit." %
              ("over the limit" if geo_bad else "within the limit", len(worst)), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
