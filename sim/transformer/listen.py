#!/usr/bin/env python3
"""Render audio through the realtime transformer model for listening.

  python3 listen.py

For each source (guitar riff, a sub-heavy bass line) and drive level, writes:
  <src>_<level>_core.wav     through the hysteretic core
  <src>_<level>_linear.wav   same transformer with an ideal linear core
  <src>_<level>_iron.wav     core minus linear, +20 dB: what the iron adds
into sim/transformer/renders/. Each core/linear pair shares one gain, so they
can be A/B'd directly.
"""
import os, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import resample_poly, butter, sosfilt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import render  # noqa: E402  (synthetic riff)

XFMR = os.path.join(HERE, "..", "..", "build", "xfmr_render")
OUT = os.path.join(HERE, "renders")
FS, OS = 48000, 4
LEVELS = {"-20dBu": -20, "0dBu": 0, "+10dBu": 10}   # source peak level


def bass_line():
    """Sub-heavy synth bass: A1 / E1 / G1 notes, sawtooth through a low-pass."""
    notes = [(55.0, 0.0, 0.9), (41.2, 1.0, 0.9), (49.0, 2.0, 0.9), (55.0, 3.0, 1.8)]
    y = np.zeros(int(5.0 * FS))
    t = np.arange(len(y)) / FS
    for f, t0, dur in notes:
        m = (t >= t0) & (t < t0 + dur)
        tt = t[m] - t0
        saw = 2 * ((f * tt) % 1) - 1
        env = np.minimum(1, tt / 0.01) * np.exp(-tt * 1.2)
        y[m] += saw * env
    return sosfilt(butter(2, 400, "lowpass", fs=FS, output="sos"), y)


def run(args):
    name, x_up, linear = args
    inp = tempfile.mktemp(suffix=".txt")
    out = tempfile.mktemp(suffix=".dat")
    t = np.arange(len(x_up)) / (FS * OS)
    np.savetxt(inp, np.column_stack([t, x_up]), fmt="%.9g")
    cmd = [XFMR, "file", inp, out] + (["--linear"] if linear else [])
    subprocess.run(cmd, capture_output=True, check=True)
    y = np.loadtxt(out, skiprows=1)[:, 1]
    os.remove(inp)
    os.remove(out)
    return name, linear, resample_poly(y, 1, OS)   # anti-aliased back to 48 kHz


def main():
    os.makedirs(OUT, exist_ok=True)
    sources = {"riff": render.synth_riff(), "bass": bass_line()}
    # the plucked-string synth leaves DC/subsonic drift in each note; any real
    # source would be high-passed, and otherwise the two cores' different
    # sub-20 Hz roll-off dominates the comparison
    hp = butter(2, 20, "highpass", fs=FS, output="sos")
    jobs = []
    for sname, x in sources.items():
        x = sosfilt(hp, x)
        x = x / np.max(np.abs(x))
        for lname, dbu in LEVELS.items():
            peak = 0.775 * np.sqrt(2) * 10 ** (dbu / 20)
            x_up = resample_poly(x * peak, OS, 1)
            for linear in (False, True):
                jobs.append((f"{sname}_{lname}", x_up, linear))
    with ProcessPoolExecutor() as ex:
        res = list(ex.map(run, jobs))
    got = {(n, lin): y for n, lin, y in res}
    print(f"{'render':16s} {'iron adds (dB, 20 Hz-20 kHz)':>30s}")
    for name in sorted({n for n, _ in got}):
        core, lin = got[(name, False)], got[(name, True)]
        n = min(len(core), len(lin))
        core, lin = core[:n], lin[:n]
        g = 0.89 / max(np.abs(core).max(), np.abs(lin).max())
        diff = sosfilt(hp, core - lin)
        rel = 20 * np.log10(np.sqrt(np.mean(diff**2)) / np.sqrt(np.mean(lin**2)))
        wavfile.write(os.path.join(OUT, f"{name}_core.wav"), FS, (core * g).astype(np.float32))
        wavfile.write(os.path.join(OUT, f"{name}_linear.wav"), FS, (lin * g).astype(np.float32))
        wavfile.write(os.path.join(OUT, f"{name}_iron.wav"), FS, np.clip(diff * g * 10, -1, 1).astype(np.float32))
        print(f"{name:16s} {rel:30.1f}")
    print(f"\nwrote {OUT}/")


if __name__ == "__main__":
    main()
