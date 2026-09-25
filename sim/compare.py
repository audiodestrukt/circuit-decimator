#!/usr/bin/env python3
"""Check the realtime C++ solver (build/ff_render) against ngspice, per preset.

  python3 compare.py                 # all presets from render.py
  python3 compare.py --only baseline wrecked

Both run the same input file at the same oversampled step. Reports DC bias
error and the error of v(out) (DC removed) relative to ngspice's RMS, and
writes renders/compare.png with overlaid zooms.
"""
import argparse, os, re, subprocess, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.signal import butter, sosfiltfilt

import render

HERE = os.path.dirname(os.path.abspath(__file__))
FF = os.path.join(HERE, "..", "build", "ff_render")


def run_cpp(name, overrides, input_txt, os_, iters):
    out = tempfile.mktemp(suffix=".dat")
    args = [FF, input_txt, out, "--fs", str(render.FS * os_), "--iters", str(iters)]
    args += [f"{k}={v}" for k, v in overrides.items()]
    log = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    bias = dict(re.findall(r"v\((\w+)\)=([-\d.e+]+)", log))
    stats = log.strip().splitlines()[-1]
    return d, {k: float(v) for k, v in bias.items()}, stats


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--iters", type=int, default=8)
    ap.add_argument("--os", type=int, default=render.OS)
    a = ap.parse_args()

    input_txt = os.path.join(HERE, "renders", ".input.txt")
    if not os.path.exists(input_txt):
        raise SystemExit("run render.py first (it writes renders/.input.txt)")
    dur = np.loadtxt(input_txt)[-1, 0]
    presets = {k: render.PRESETS[k] for k in (a.only or render.PRESETS)}

    with ProcessPoolExecutor() as ex:
        spice = dict((r[0], r) for r in ex.map(
            render.simulate, [(n, o, input_txt, dur, a.os) for n, o in presets.items()]))

    fs = render.FS * a.os
    hp = butter(2, 15, "highpass", fs=fs, output="sos")
    rows = []
    print(f"{'preset':17s} {'Vc2 spice':>9s} {'Vc2 C++':>8s} {'err/rms':>8s}  C++ stats")
    for name, ov in presets.items():
        _, ds, bias_s, _, err = spice[name]
        if ds is None:
            print(f"{name:17s} ngspice failed"); continue
        dc, bias_c, stats = run_cpp(name, ov, input_txt, a.os, a.iters)
        grid = dc[:, 0]
        ys = sosfiltfilt(hp, np.interp(grid, ds[:, 0], ds[:, 1]))
        yc = sosfiltfilt(hp, dc[:, 1])
        rel = np.sqrt(np.mean((ys - yc) ** 2)) / (np.sqrt(np.mean(ys ** 2)) + 1e-12)
        print(f"{name:17s} {bias_s.get('c2', np.nan):8.3f}V {bias_c.get('c2', np.nan):7.3f}V "
              f"{rel * 100:7.1f}%  {stats.split(':', 1)[1].strip()}")
        rows.append((name, grid, ys, yc))

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(len(rows), 1, figsize=(10, 1.4 * len(rows)), sharex=True)
    axes = np.atleast_1d(axes)
    for ax, (name, t, ys, yc) in zip(axes, rows):
        m = (t > 2.05) & (t < 2.07)
        ax.plot(t[m] * 1e3, ys[m], lw=1.6, color="0.7", label="ngspice")
        ax.plot(t[m] * 1e3, yc[m], lw=0.7, color="tab:red", label="C++")
        ax.set_ylabel(name, rotation=0, ha="right", fontsize=8)
        ax.tick_params(labelsize=6)
    axes[0].legend(fontsize=7, loc="upper right")
    axes[-1].set_xlabel("ms")
    fig.tight_layout()
    fig.savefig(os.path.join(HERE, "renders", "compare.png"), dpi=110)


if __name__ == "__main__":
    main()
