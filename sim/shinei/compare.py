#!/usr/bin/env python3
"""Shin-Ei FY-2: ngspice (shinei.cir) vs the general engine (build/circuit_render shinei).

  python3 compare.py                    # all presets on the synthetic riff
  python3 compare.py --only baseline starve_4v5
  python3 compare.py --input di.wav     # your own DI (mono-mixed, 150 mV peak)

Both run the same input at 4x 48 kHz. Reports the DC bias of both collectors,
the error of v(out) (DC removed) relative to ngspice's RMS, the engine's solver
stats, and writes renders/<preset>_{spice,engine}.wav plus renders/compare.png.
The baseline row also prints the output scale that puts its peak at -1 dBFS
(cd::kOutputScaleShinEi in core/circuit/Knobs.h).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfiltfilt, resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import render  # noqa: E402  (fuzz deck harness: riff synth, input loading)

os.environ["OMP_NUM_THREADS"] = "1"   # ngspice OpenMP makes runs non-deterministic

DECK = os.path.join(HERE, "shinei.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "circuit_render")
FS, OS = render.FS, render.OS

# Each preset overrides .param defaults in the deck ("temp" -> .options temp).
PRESETS = {
    "baseline":        {},
    "fuzz_low":        {"fuzz": 0.15},
    "starve_6v":       {"vcc": 6},
    "starve_4v5":      {"vcc": 4.5},
    "dying_battery":   {"vcc": 7.5, "rbat": 3e3, "cbulk": 4.7e-6},
    "bias_cold":       {"rb2": 1.8e6},
    "weak_q1":         {"bf1": 40},
    "leaky_junctions": {"rleak1": 2e6, "rleak2": 2e6},
    "hot_day":         {"temp": 70},
    "frozen":          {"temp": -20},
}


def build_deck(overrides, dur):
    txt = open(DECK).read()
    unused = dict(overrides)
    for k, v in overrides.items():
        if k == "temp":
            txt, n = re.subn(r"(?m)^\.options temp=\S+", f".options temp={v:.6g}", txt)
        else:
            txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
        if n:
            unused.pop(k)
    if unused:
        raise SystemExit(f"unknown param(s) for deck: {sorted(unused)}")
    ts = f"{1 / (FS * OS):.6g}"
    txt = re.sub(r"(?m)^tran .*", f"tran {ts} {dur:.6g} 0 {ts}", txt)
    return txt


def spice(args):
    name, overrides, input_txt, dur = args
    work = tempfile.mkdtemp(prefix=f"se_{name}_")
    try:
        shutil.copy(input_txt, os.path.join(work, "input.txt"))
        open(os.path.join(work, "deck.cir"), "w").write(build_deck(overrides, dur))
        t0 = time.perf_counter()
        r = subprocess.run(["ngspice", "-b", "deck.cir"], cwd=work, capture_output=True, text=True)
        wall = time.perf_counter() - t0
        log = r.stdout + r.stderr
        outp = os.path.join(work, "out.dat")
        if not os.path.exists(outp):
            return name, None, None, wall, log[-2000:]
        d = np.loadtxt(outp, skiprows=1)
        bias = {m[0]: float(m[1]) for m in re.findall(r"^v\((\w+)\)\s*=\s*([-\d.e+]+)", log, re.M)}
        return name, d, bias, wall, None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def engine(args):
    name, overrides, input_txt = args
    out = tempfile.mktemp(suffix=".dat")
    cmd = [TOOL, "shinei", "file", input_txt, out, "--fs", str(FS * OS)]
    for k, v in overrides.items():
        cmd += ["--set", f"{k}={v}"]
    log = subprocess.run(cmd, capture_output=True, text=True, check=True).stdout
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    bias = {k: float(v) for k, v in re.findall(r"v\((\w+)\)=([-\d.e+]+)", log)}
    return name, d, bias, log.strip().splitlines()[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--input")
    a = ap.parse_args()
    presets = {k: PRESETS[k] for k in (a.only or PRESETS)}
    if "baseline" not in presets:
        presets = {"baseline": {}, **presets}

    out_dir = os.path.join(HERE, "renders")
    os.makedirs(out_dir, exist_ok=True)
    x = render.load_input(a.input)
    dur = len(x) / FS
    input_txt = os.path.join(out_dir, ".input.txt")
    np.savetxt(input_txt, np.column_stack([np.arange(len(x)) / FS, x]), fmt="%.9g")
    wavfile.write(os.path.join(out_dir, "input.wav"), FS, (x / np.max(np.abs(x)) * 0.89).astype(np.float32))

    with ProcessPoolExecutor() as ex:
        sp = {r[0]: r for r in ex.map(spice, [(n, o, input_txt, dur) for n, o in presets.items()])}
        en = {r[0]: r for r in ex.map(engine, [(n, o, input_txt) for n, o in presets.items()])}

    fs = FS * OS
    hp = butter(2, 15, "highpass", fs=fs, output="sos")
    rows, gain = [], None
    print(f"{'preset':17s} {'Vc1 spice/eng':>16s} {'Vc2 spice/eng':>16s} {'err/rms':>8s}  engine")
    for name in presets:
        _, ds, bs, wall, err = sp[name]
        if ds is None:
            print(f"{name:17s} ngspice failed\n{err}")
            continue
        _, de, be, stats = en[name]
        grid = de[:, 0]
        ys = sosfiltfilt(hp, np.interp(grid, ds[:, 0], ds[:, 1]))
        ye = sosfiltfilt(hp, de[:, 1])
        rel = np.sqrt(np.mean((ys - ye) ** 2)) / (np.sqrt(np.mean(ys ** 2)) + 1e-12)
        print(f"{name:17s} {bs.get('c1', np.nan):7.3f}/{be.get('c1', np.nan):7.3f} "
              f"{bs.get('c2', np.nan):7.3f}/{be.get('c2', np.nan):7.3f} {rel * 100:7.2f}%  "
              f"{stats.split(':', 1)[1].strip()}  (ngspice {dur / wall:.1f}x)")
        if name == "baseline":
            peak = np.max(np.abs(ye))
            gain = 0.89 / peak
            print(f"{'':17s} baseline engine peak {peak:.4f} V -> output scale {gain:.3f} for -1 dBFS")
        for tag, y in (("spice", ys), ("engine", ye)):
            wav = np.clip(resample_poly(y, 1, OS) * gain, -1, 1)
            wavfile.write(os.path.join(out_dir, f"{name}_{tag}.wav"), FS, wav.astype(np.float32))
        rows.append((name, grid, ys, ye))

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(len(rows), 1, figsize=(10, 1.4 * len(rows)), sharex=True)
    axes = np.atleast_1d(axes)
    for ax, (name, t, ys, ye) in zip(axes, rows):
        m = (t > 2.05) & (t < 2.07)
        ax.plot(t[m] * 1e3, ys[m], lw=1.6, color="0.7", label="ngspice")
        ax.plot(t[m] * 1e3, ye[m], lw=0.7, color="tab:red", label="engine")
        ax.set_ylabel(name, rotation=0, ha="right", fontsize=8)
        ax.tick_params(labelsize=6)
    axes[0].legend(fontsize=7, loc="upper right")
    axes[-1].set_xlabel("ms")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "compare.png"), dpi=110)
    print(f"\nwrote {out_dir}/")


if __name__ == "__main__":
    main()
