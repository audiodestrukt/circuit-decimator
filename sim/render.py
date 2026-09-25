#!/usr/bin/env python3
"""Render audio through fuzzface.cir with ngspice, one WAV per "destroy" preset.

  python3 render.py                       # all presets, synthetic guitar riff
  python3 render.py --input di.wav        # your own DI recording (mono-mixed)
  python3 render.py --only baseline starve_4v5
  python3 render.py --set vcc=5 --set bf1=40 --name my_wreck

Output: renders/<preset>.wav (+ renders/input.wav, renders/summary.png).
All presets share one gain (set so baseline peaks at -1 dBFS), so a starved
circuit really is quieter -- that's part of the effect.
"""
import argparse, math, os, re, shutil, subprocess, sys, tempfile, time
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, sosfilt, resample_poly

HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.join(HERE, "fuzzface.cir")
FS = 48000
# ngspice steps at FS*OS; output is band-limited and decimated (fuzz = square
# waves, so sampling straight at FS aliases the harmonics)
OS = 4

# Each preset overrides .param defaults in the deck ("temp" -> .options temp).
PRESETS = {
    "baseline":        {},
    "fuzz_low":        {"fuzz": 0.15},
    "starve_6v":       {"vcc": 6},
    "starve_4v5":      {"vcc": 4.5},
    "starve_3v6":      {"vcc": 3.6},    # edge of death: Q2 ~saturated
    "dying_battery":   {"vcc": 7.5, "rbat": 3e3, "cbulk": 4.7e-6},
    "bias_cold":       {"rc2b": 8.2e3},    # Q2 near saturation: gated, splatty
    "bias_hot":        {"rc2b": 3.3e3},    # Q2 near cutoff: thin, clipped one side
    "weak_q1":         {"bf1": 40},
    "leaky_junctions": {"rleak1": 300e3, "rleak2": 300e3},
    "leaky_input_cap": {"rleak_cin": 100e3},  # DC pulls Q1 base via pickup
    "dried_bypass":    {"cfz": 1e-6},
    "hot_day":         {"temp": 70},
    "frozen":          {"temp": -20},
    "wrecked":         {"vcc": 4, "rbat": 800, "cbulk": 22e-6, "bf1": 40,
                        "rleak2": 150e3, "temp": 55, "cfz": 2e-6},
}


# ---------------------------------------------------------------- input audio
def pluck(freq, dur, fs=FS, bright=0.5, seed=0):
    """Karplus-Strong string: noise burst through a damped delay loop."""
    rng = np.random.default_rng(seed)
    n = int(dur * fs)
    period = fs / freq
    p = int(period)
    frac = period - p
    buf = rng.uniform(-1, 1, p + 2)
    # soften the excitation (pick position / brightness)
    for _ in range(int((1 - bright) * 4)):
        buf = 0.5 * (buf + np.roll(buf, 1))
    out = np.zeros(n)
    decay = 0.996
    y = np.concatenate([buf, np.zeros(n)])
    for i in range(p + 2, n + p + 2):
        a = y[i - p - 1] * (1 - frac) + y[i - p - 2] * frac
        b = y[i - p] * (1 - frac) + y[i - p - 1] * frac
        y[i] = decay * 0.5 * (a + b)
    out[:] = y[p + 2 : p + 2 + n]
    return out


def synth_riff():
    """~5 s: strummed E5, a chugged riff, then a ringing A5 to hear the decay/gate."""
    notes = lambda *m: [440 * 2 ** ((k - 69) / 12) for k in m]
    total = np.zeros(int(5.0 * FS))

    def place(t0, freqs, dur, amp, strum=0.012):
        for j, f in enumerate(freqs):
            s = pluck(f, dur, seed=int(f * 7 + t0 * 100)) * amp
            i0 = int((t0 + j * strum) * FS)
            seg = total[i0 : i0 + len(s)]
            seg += s[: len(seg)]

    place(0.00, notes(40, 47, 52), 1.6, 0.35)               # E5 chord
    riff = [40, 40, 43, 45, 40, 40, 46, 45]                  # E E G A E E Bb A
    for k, m in enumerate(riff):
        t = 1.7 + k * 0.2
        place(t, notes(m), 0.19, 0.5)                        # staccato (damped)
    place(3.4, notes(45, 52, 57), 1.6, 0.35)                 # A5, let ring
    # pickup EMF: ~150 mV peaks, like a humbucker-ish single-coil
    return 0.15 * total / np.max(np.abs(total))


def load_input(path):
    if path is None:
        return synth_riff()
    fs, x = wavfile.read(path)
    x = x.astype(np.float64)
    if x.ndim > 1:
        x = x.mean(axis=1)
    if fs != FS:
        g = math.gcd(fs, FS)
        x = resample_poly(x, FS // g, fs // g)
    # scale to pickup level: DI recordings vary, so normalise to 150 mV peak
    return 0.15 * x / np.max(np.abs(x))


# ---------------------------------------------------------------- deck + sim
def fmt(v):
    return f"{v:.6g}"


def build_deck(overrides, dur, os_=OS):
    txt = open(DECK).read()
    unused = dict(overrides)
    for k, v in overrides.items():
        if k == "temp":
            txt, n = re.subn(r"(?m)^\.options temp=\S+", f".options temp={fmt(v)}", txt)
        else:
            txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{fmt(v)}", txt)
        if n:
            unused.pop(k)
    if unused:
        raise SystemExit(f"unknown param(s) for deck: {sorted(unused)}")
    ts = fmt(1 / (FS * os_))
    txt = re.sub(r"(?m)^tran .*", f"tran {ts} {fmt(dur)} 0 {ts}", txt)
    return txt


def simulate(args):
    name, overrides, input_txt, dur, os_ = args
    work = tempfile.mkdtemp(prefix=f"ff_{name}_")
    try:
        shutil.copy(input_txt, os.path.join(work, "input.txt"))
        with open(os.path.join(work, "deck.cir"), "w") as f:
            f.write(build_deck(overrides, dur, os_))
        t0 = time.perf_counter()
        r = subprocess.run(["ngspice", "-b", "deck.cir"], cwd=work,
                           capture_output=True, text=True)
        wall = time.perf_counter() - t0
        log = r.stdout + r.stderr
        outp = os.path.join(work, "out.dat")
        if not os.path.exists(outp):
            return name, None, None, wall, log[-2000:]
        d = np.loadtxt(outp, skiprows=1)
        bias = {m[0]: float(m[1]) for m in
                re.findall(r"^v\((\w+)\)\s*=\s*([-\d.e+]+)", log, re.M)}
        return name, d, bias, wall, None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def to_audio(d, dur, os_):
    """Put ngspice output on the oversampled grid, anti-alias + decimate to FS,
    strip DC like an amp input would."""
    t = d[:, 0]
    n = int(dur * FS)
    grid = np.arange(n * os_) / (FS * os_)
    y = resample_poly(np.interp(grid, t, d[:, 1]), 1, os_)[:n]
    sos = butter(2, 15, "highpass", fs=FS, output="sos")
    grid = grid[::os_][:n]
    return sosfilt(sos, y), np.interp(grid, t, d[:, 2]), np.interp(grid, t, d[:, 3])


# ---------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--input", help="DI wav (default: synthetic riff)")
    ap.add_argument("--only", nargs="*", help="preset names to render")
    ap.add_argument("--set", action="append", default=[], metavar="K=V",
                    help="custom override (repeatable); renders one file")
    ap.add_argument("--name", default="custom")
    ap.add_argument("--out", default=os.path.join(HERE, "renders"))
    ap.add_argument("-j", type=int, default=os.cpu_count())
    ap.add_argument("--os", type=int, default=OS, help=f"oversampling (default {OS})")
    a = ap.parse_args()

    if a.set:
        jobs = {a.name: {k: float(v) for k, v in (s.split("=", 1) for s in a.set)}}
    else:
        jobs = {k: PRESETS[k] for k in (a.only or PRESETS)}
    # baseline always rendered: it sets the shared output gain
    jobs = {"baseline": {}, **jobs}

    os.makedirs(a.out, exist_ok=True)
    x = load_input(a.input)
    dur = len(x) / FS
    wavfile.write(os.path.join(a.out, "input.wav"), FS,
                  (x / np.max(np.abs(x)) * 0.89).astype(np.float32))
    input_txt = os.path.join(a.out, ".input.txt")
    t = np.arange(len(x)) / FS
    np.savetxt(input_txt, np.column_stack([t, x]), fmt="%.9g")

    print(f"input {dur:.2f} s @ {FS} Hz (sim {a.os}x oversampled), "
          f"{len(jobs)} renders on {a.j} procs")
    with ProcessPoolExecutor(a.j) as ex:
        results = list(ex.map(simulate, [(n, o, input_txt, dur, a.os) for n, o in jobs.items()]))

    rendered = {}
    for name, d, bias, wall, err in results:
        if d is None:
            print(f"  {name:17s} FAILED\n{err}")
            continue
        y, vp, vc2 = to_audio(d, dur, a.os)
        rendered[name] = (y, vp, vc2, bias, wall)

    if "baseline" not in rendered:
        sys.exit("baseline failed; can't set output gain")
    gain = 0.89 / np.max(np.abs(rendered["baseline"][0]))

    print(f"\n  {'preset':17s} {'Vc2 bias':>8s} {'peak dBFS':>9s} {'rms dBFS':>8s} "
          f"{'sim s':>6s} {'x realtime':>10s}")
    for name, (y, vp, vc2, bias, wall) in rendered.items():
        out = y * gain
        clip = np.max(np.abs(out)) > 1
        if clip:
            out = np.tanh(out)  # never write >0 dBFS; flag it instead
        wavfile.write(os.path.join(a.out, f"{name}.wav"), FS, out.astype(np.float32))
        pk = 20 * np.log10(np.max(np.abs(y * gain)) + 1e-12)
        rms = 20 * np.log10(np.sqrt(np.mean((y * gain) ** 2)) + 1e-12)
        print(f"  {name:17s} {bias.get('c2', float('nan')):7.2f}V {pk:9.1f} {rms:8.1f} "
              f"{wall:6.1f} {dur / wall:9.1f}x" + ("  (soft-clipped)" if clip else ""))

    plot(rendered, gain, x, dur, os.path.join(a.out, "summary.png"))
    print(f"\nwrote {a.out}/")


def plot(rendered, gain, x, dur, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    n = len(rendered)
    fig, axes = plt.subplots(n + 1, 2, figsize=(13, 1.3 * (n + 1)), sharex="col",
                             gridspec_kw={"width_ratios": [3, 1]})
    axes = np.atleast_2d(axes)
    t = np.arange(len(x)) / FS
    zoom = (t > 2.05) & (t < 2.08)  # a few cycles inside the riff
    rows = [("input", x / np.max(np.abs(x)), None)] + \
           [(k, v[0] * gain, v[1]) for k, v in rendered.items()]
    for ax, (name, y, vp) in zip(axes, rows):
        ax[0].plot(t, y, lw=0.3, color="k")
        if vp is not None and np.ptp(vp) > 0.05:
            ax2 = ax[0].twinx()
            ax2.plot(t, vp, lw=0.8, color="tab:red")
            ax2.set_ylabel("Vsupply", color="tab:red", fontsize=6)
            ax2.tick_params(labelsize=6)
        ax[0].set_ylim(-1.1, 1.1)
        ax[0].set_ylabel(name, rotation=0, ha="right", fontsize=8)
        ax[0].tick_params(labelsize=6)
        ax[1].plot(t[zoom] * 1e3, y[zoom], lw=0.8, color="k")
        ax[1].set_ylim(-1.1, 1.1)
        ax[1].tick_params(labelsize=6)
    axes[-1][0].set_xlabel("s")
    axes[-1][1].set_xlabel("ms (zoom)")
    fig.tight_layout()
    fig.savefig(path, dpi=110)


if __name__ == "__main__":
    main()
