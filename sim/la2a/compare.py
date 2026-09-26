#!/usr/bin/env python3
"""LA-2A: ngspice (la2a.cir, one circuit) vs the engine (build/la2a_render:
audio path + sidechain solved separately).

A 1 kHz tone steps from lv1 up to lv2 dBu at t1 and back at t2. Compared:
per-cycle output level (dB) through attack, hold and release; gain reduction
at the end of the hold; waveform error in steady state.

  python3 compare.py                       # the default cases
  python3 compare.py --case -30 10 --peak 0.7
  python3 compare.py --plot envelopes.png
"""
import argparse, os, re, subprocess, tempfile
import numpy as np

os.environ["OMP_NUM_THREADS"] = "1"   # ngspice's OpenMP threading makes runs non-deterministic
HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.join(HERE, "la2a.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "la2a_render")
FS = 96000
T1, T2, DUR = 0.3, 1.3, 2.5


def spice(lv1, lv2, peak, gain):
    over = {"lv1": lv1, "lv2": lv2, "t1": T1, "t2": T2, "peak": peak, "gain": gain}
    txt = open(DECK).read()
    for k, v in over.items():
        txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
        assert n, k
    work = tempfile.mkdtemp(prefix="la2a_")
    open(os.path.join(work, "d.cir"), "w").write(txt)
    r = subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True, text=True)
    path = os.path.join(work, "la2a.dat")
    if not os.path.exists(path):
        raise RuntimeError(r.stdout[-2000:] + r.stderr[-2000:])
    d = np.loadtxt(path, skiprows=1)
    return d[:, 0], d[:, 1], d[:, 2]


def engine(lv1, lv2, peak, gain, every=1):
    out = tempfile.mktemp(suffix=".dat")
    subprocess.run([TOOL, out, "--lv1", str(lv1), "--lv2", str(lv2), "--t1", str(T1), "--t2", str(T2),
                    "--dur", str(DUR), "--fs", str(FS), "--peak", str(peak), "--gain", str(gain),
                    "--every", str(every)], check=True, capture_output=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return d[:, 0], d[:, 1], d[:, 2]


def envelope(t, y, period=1e-3):
    """peak level per cycle, dBu"""
    edges = np.arange(0, t[-1], period)
    idx = np.searchsorted(t, edges)
    pk = np.array([np.max(np.abs(y[a:b])) if b > a else np.nan for a, b in zip(idx[:-1], idx[1:])])
    return edges[:-1], 20 * np.log10(np.maximum(pk, 1e-9) / (0.775 * np.sqrt(2)))


def metrics(te, env):
    hold = env[(te > T2 - 0.05) & (te < T2)].mean()
    pre = env[(te > T1 - 0.05) & (te < T1)].mean()
    after = env[te >= T2]
    ta = te[te >= T2]
    # attack: from the step to within 1 dB of the hold level (after the overshoot)
    seg = env[(te >= T1) & (te < T2)]
    ts_ = te[(te >= T1) & (te < T2)]
    above = np.nonzero(np.abs(seg - hold) > 1.0)[0]
    attack = (ts_[above[-1]] - T1) if len(above) else 0.0
    # release: from the step down until the level is back within 1 dB of the pre-step level
    rec = np.nonzero(after < pre - 1.0)[0]
    release = (ta[rec[-1]] - T2) if len(rec) else 0.0
    return pre, hold, attack, release


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", nargs=2, type=float, action="append")
    ap.add_argument("--peak", type=float, default=0.5)
    ap.add_argument("--gain", type=float, default=0.75)
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--plot")
    a = ap.parse_args()
    cases = a.case or [(-30, -10), (-30, 0), (-30, 10)]
    curves = []
    print(f"Peak Reduction {a.peak}, Gain {a.gain}, sidechain every {a.every}; tone 1 kHz, step at {T1} s, back at {T2} s")
    print("  step dBu      out before (dBu)   out at hold (dBu)   attack to 1 dB (ms)   release to 1 dB (s)   env diff dB rms/max   wave err")
    for lv1, lv2 in cases:
        ts, ys, _ = spice(lv1, lv2, a.peak, a.gain)
        te, ye, _ = engine(lv1, lv2, a.peak, a.gain, a.every)
        es = envelope(ts, ys)
        ee = envelope(te, ye)
        n = min(len(es[1]), len(ee[1]))
        d = es[1][:n] - ee[1][:n]
        ms, me = metrics(*es), metrics(*ee)
        # waveform error in the hold's last 50 ms, on a common time grid
        tg = np.arange(T2 - 0.05, T2, 1 / FS)
        yi = np.interp(tg, ts, ys)
        ei = np.interp(tg, te, ye)
        werr = np.sqrt(np.mean((yi - ei) ** 2)) / np.sqrt(np.mean(yi ** 2))
        print(f"  {lv1:+4.0f}->{lv2:+4.0f}   {ms[0]:6.2f} / {me[0]:6.2f}      {ms[1]:6.2f} / {me[1]:6.2f}       "
              f"{ms[2]*1e3:6.1f} / {me[2]*1e3:6.1f}        {ms[3]:5.2f} / {me[3]:5.2f}          "
              f"{np.sqrt(np.mean(d**2)):5.2f} / {np.max(np.abs(d)):5.2f}      {werr:.2e}")
        curves.append((lv1, lv2, es, ee))
    print("  (ngspice / engine)")
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(figsize=(10, 5))
        for lv1, lv2, es, ee in curves:
            l, = ax.plot(es[0], es[1], lw=2.5, alpha=0.45, label=f"ngspice {lv1:+.0f}->{lv2:+.0f} dBu")
            ax.plot(ee[0], ee[1], lw=1, color=l.get_color(), label=f"engine {lv1:+.0f}->{lv2:+.0f} dBu")
        ax.set_xlabel("time (s)")
        ax.set_ylabel("output level (dBu peak/cycle)")
        ax.set_title(f"LA-2A tone-step response, Peak Reduction {a.peak}")
        ax.grid(alpha=0.3)
        ax.legend(fontsize=8)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
