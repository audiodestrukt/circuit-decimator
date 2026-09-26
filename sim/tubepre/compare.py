#!/usr/bin/env python3
"""Tube mic pre: ngspice (tubepre.cir) vs the general engine (build/tubepre_render).

  python3 compare.py            # gain + THD vs mic level at 50 Hz and 1 kHz, both solvers
  python3 compare.py --listen   # also render riff / bass at mic levels, core vs linear iron

Levels are the mic's open-circuit peak voltage in dBu (a dynamic mic on a loud
source is roughly -50 to -30 dBu; -20 dBu is a very loud close-miked source).
"""
import argparse, os, re, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, resample_poly, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transformer"))
sys.path.insert(0, os.path.join(HERE, ".."))
from core_sweep import thd  # noqa: E402
import render  # noqa: E402

# ngspice's OpenMP threading makes runs non-deterministic (different timestep
# sequences, occasionally a wrong trajectory), worst when many run in parallel.
os.environ["OMP_NUM_THREADS"] = "1"

DECK = os.path.join(HERE, "tubepre.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "tubepre_render")
DBU = 0.775 * np.sqrt(2)


def spice(args):
    """ngspice is occasionally non-deterministic on this deck: identical runs can
    take different timestep sequences and, now and then, a wrong trajectory
    (e.g. 4.8% THD where the answer is 0.06%). Run twice; if they disagree, run
    a third time and keep the median, so a single bad run can't pose as the
    reference. The engine is deterministic."""
    runs = [spice_once(args), spice_once(args)]
    t = [thd(r[:, 0], r[:, 1], args[1])[0] for r in runs]
    if abs(t[0] - t[1]) > 0.01 * max(t[0], t[1], 1e-6):
        runs.append(spice_once(args))
        t.append(thd(runs[2][:, 0], runs[2][:, 1], args[1])[0])
        return runs[int(np.argsort(t)[1])]
    return runs[0]


def spice_once(args):
    amp, freq = args
    txt = open(DECK).read()
    for k, v in {"amp": amp, "freq": freq}.items():
        txt = re.sub(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
    dt = min(1 / (freq * 400), 1 / 192000)
    txt = re.sub(r"(?m)^tran .*", f"tran {dt:.6g} {12 / freq:.6g} 0 {dt:.6g}", txt)
    work = tempfile.mkdtemp(prefix="tp_")
    open(os.path.join(work, "d.cir"), "w").write(txt)
    subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True)
    return np.loadtxt(os.path.join(work, "tube.dat"), skiprows=1)


def engine(args):
    amp, freq = args
    out = tempfile.mktemp(suffix=".dat")
    subprocess.run([TOOL, "sine", str(amp), str(freq), "12", out], capture_output=True, check=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return d


def render_file(args):
    name, x_up, linear = args
    inp, out = tempfile.mktemp(suffix=".txt"), tempfile.mktemp(suffix=".dat")
    np.savetxt(inp, np.column_stack([np.arange(len(x_up)) / 192000, x_up]), fmt="%.9g")
    subprocess.run([TOOL, "file", inp, out] + (["--linear"] if linear else []), capture_output=True, check=True)
    y = np.loadtxt(out, skiprows=1)[:, 1]
    os.remove(inp)
    os.remove(out)
    return name, linear, resample_poly(y, 1, 4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--listen", action="store_true")
    a = ap.parse_args()

    levels_dbu = [-60, -50, -40, -30, -20, -10]
    freqs = [50, 1000]
    jobs = [(DBU * 10 ** (l / 20), f) for f in freqs for l in levels_dbu]
    # ngspice sequentially: run one at a time it is deterministic and matches the
    # engine; many concurrent ngspice processes on these hysteresis decks
    # occasionally take a wrong trajectory (see README)
    sp = [spice(j) for j in jobs]
    with ProcessPoolExecutor() as ex:
        en = list(ex.map(engine, jobs))

    print(f"{'mic dBu':>8s} {'freq':>6s} | {'gain dB (spice / engine)':>26s} | {'THD % (spice / engine)':>24s}")
    for (amp, f), ds, de in zip(jobs, sp, en):
        ts, os_, es, fs_ = thd(ds[:, 0], ds[:, 1], f)
        te, oe, ee, fe = thd(de[:, 0], de[:, 1], f)
        print(f"{20 * np.log10(amp / DBU):8.0f} {f:6d} | {20 * np.log10(fs_ / amp):12.2f} / {20 * np.log10(fe / amp):8.2f}   | "
              f"{ts:10.3f} / {te:8.3f}   (odd/even engine {oe:.2f}/{ee:.2f})")

    if not a.listen:
        return
    out_dir = os.path.join(HERE, "renders")
    os.makedirs(out_dir, exist_ok=True)
    hp = butter(2, 20, "highpass", fs=48000, output="sos")
    sys.path.insert(0, os.path.join(HERE, "..", "transformer"))
    from listen import bass_line  # noqa: E402
    sources = {"riff": render.synth_riff(), "bass": bass_line()}
    lvls = {"-50dBu": -50, "-35dBu": -35, "-20dBu": -20}
    jobs = []
    for sname, x in sources.items():
        x = sosfilt(hp, x)
        x = x / np.max(np.abs(x))
        for lname, l in lvls.items():
            x_up = resample_poly(x * DBU * 10 ** (l / 20), 4, 1)
            for linear in (False, True):
                jobs.append((f"{sname}_{lname}", x_up, linear))
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(render_file, jobs))
    got = {(n, lin): y for n, lin, y in res}
    print(f"\n{'render':14s} {'out peak dBu':>13s} {'iron adds (dB)':>15s}")
    for name in sorted({n for n, _ in got}):
        core, lin = got[(name, False)], got[(name, True)]
        core = sosfilt(hp, core)
        lin = sosfilt(hp, lin)
        g = 0.89 / max(np.abs(core).max(), np.abs(lin).max())
        diff = core - lin
        rel = 20 * np.log10(np.sqrt(np.mean(diff**2)) / np.sqrt(np.mean(lin**2)))
        wavfile.write(os.path.join(out_dir, f"{name}_core.wav"), 48000, (core * g).astype(np.float32))
        wavfile.write(os.path.join(out_dir, f"{name}_linear.wav"), 48000, (lin * g).astype(np.float32))
        pk = 20 * np.log10(np.abs(core).max() / DBU)
        print(f"{name:14s} {pk:13.1f} {rel:15.1f}")
    print(f"\nwrote {out_dir}/")


if __name__ == "__main__":
    main()
