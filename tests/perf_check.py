#!/usr/bin/env python3
"""CPU regression check for the realtime workloads (tools/perf_bench.cpp).

Runs perf_bench, compares each benchmark with tests/perf_baseline.json for this
machine, and fails on a regression:
  - normalised CPU cost (the benchmark's CPU time / a fixed reference workload
    timed in the same run) more than --tolerance above the baseline (default 25 %)
  - Newton iterations per sample more than 2 % above the baseline (deterministic)
Faster than the baseline by more than the tolerance is reported (not a failure):
consider --update so the gain is locked in.

A machine with no entry in the baseline reports its numbers and passes with a
warning; --update records one. Machines: --machine, else the hostname.

  python3 tests/perf_check.py --bench build/perf_bench [--machine NAME]
      [--tolerance 0.25] [--update] [--history tests/perf_history.jsonl] [--json out.json]
"""
import argparse, datetime, json, os, socket, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASELINE = os.path.join(HERE, "perf_baseline.json")
ITER_TOL = 0.02


def git_rev():
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=HERE, capture_output=True,
                              text=True).stdout.strip()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench", required=True)
    ap.add_argument("--machine", default=socket.gethostname())
    ap.add_argument("--tolerance", type=float, default=0.25)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--history")
    ap.add_argument("--json")
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()

    r = subprocess.run([a.bench] + (["--quick"] if a.quick else []), capture_output=True, text=True, check=True)
    run = json.loads(r.stdout)
    if a.json:
        open(a.json, "w").write(r.stdout)
    base_all = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
    base = base_all.get(a.machine)

    if a.history:
        with open(a.history, "a") as h:
            h.write(json.dumps({"time": datetime.datetime.now().isoformat(timespec="seconds"), "rev": git_rev(),
                                "machine": a.machine,
                                "normalized": {k: v["normalized"] for k, v in run["benchmarks"].items()},
                                "iterations": {k: v["iterations"] for k, v in run["benchmarks"].items()
                                               if v["iterations"] >= 0}}) + "\n")

    fails, gains = [], []
    print(f"machine {a.machine}, git {git_rev() or '?'}; tolerance +{a.tolerance:.0%} (CPU), +{ITER_TOL:.0%} (Newton iterations)")
    print(f"  {'benchmark':22s} {'unit':12s} {'time':>10s} {'normalised':>11s} {'baseline':>10s} {'change':>8s}   iterations")
    for k, v in run["benchmarks"].items():
        if k == "reference":
            continue
        t = v["seconds"]
        tstr = f"{t*1e9:8.1f} ns" if t < 1e-3 else f"{t*1e3:8.1f} ms"
        line = f"  {k:22s} {v['unit']:12s} {tstr:>10s} {v['normalized']:11.4g}"
        b = base.get(k) if base else None
        if b:
            ch = v["normalized"] / b["normalized"] - 1
            line += f" {b['normalized']:10.4g} {ch:+7.1%}"
            if ch > a.tolerance:
                fails.append(f"{k}: CPU {ch:+.0%} over the baseline")
                line += "  REGRESSION"
            elif ch < -a.tolerance:
                gains.append(k)
        else:
            line += f" {'-':>10s} {'-':>8s}"
        if v["iterations"] >= 0:
            line += f"   {v['iterations']:.4f}"
            if b and b.get("iterations", -1) >= 0 and v["iterations"] > b["iterations"] * (1 + ITER_TOL):
                fails.append(f"{k}: Newton iterations {v['iterations']:.4f} vs {b['iterations']:.4f}")
                line += "  REGRESSION"
        print(line)

    if a.update:
        base_all[a.machine] = {k: {"normalized": v["normalized"], "iterations": v["iterations"]}
                               for k, v in run["benchmarks"].items() if k != "reference"}
        json.dump(base_all, open(BASELINE, "w"), indent=1, sort_keys=True)
        print(f"baseline for {a.machine} written to {BASELINE}")
        return 0
    if base is None:
        print(f"WARNING: no baseline for machine '{a.machine}': nothing compared (record one with --update)")
        return 0
    if gains:
        print(f"note: faster than the baseline by more than {a.tolerance:.0%}: {', '.join(gains)} (--update to lock it in)")
    if fails:
        print("FAILED:\n  " + "\n  ".join(fails))
        return 1
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
