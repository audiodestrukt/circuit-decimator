#!/usr/bin/env python3
"""The whole speaker with cone breakup: lumped driver + box (analytic network, as
verified in compare.py) x cone breakup + radiation (build/radiation_check).

  left:  SPL at 1 m on axis, 2.83 V -- the speaker's "datasheet" curve -- rigid vs breakup
  right: a close mic (cardioid, 2 cm capsule, 2.5 cm) from the dust cap to the edge, with breakup

  python3 breakup.py [--plot out.png] [radiation_check options, e.g. --eta 0.08 --sr 20]
"""
import argparse, os, subprocess, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compare import P, analytic  # noqa: E402

TOOL = os.path.join(HERE, "..", "..", "build", "radiation_check")


def spectrum(*args, fs=48000, n=4096):
    r = subprocess.run([TOOL, "spectrum", "--fs", str(fs), "--n", str(n), *map(str, args)],
                       capture_output=True, text=True, check=True)
    d = np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])
    return d[:, 0], d[:, 1] + 1j * d[:, 2]


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def smooth(f, y, frac=1 / 6):
    """fractional-octave smoothing of a dB curve (power average)"""
    p = 10 ** (y / 10)
    out = np.empty_like(y)
    for i, fc in enumerate(f):
        m = (f > fc * 2 ** (-frac / 2)) & (f < fc * 2 ** (frac / 2))
        out[i] = 10 * np.log10(np.mean(p[m])) if m.any() else y[i]
    return out


def main():
    ap = argparse.ArgumentParser(allow_abbrev=False, add_help=False)
    ap.add_argument("--plot")
    a, extra = ap.parse_known_args()
    f, rigid = spectrum("--distance", 1.0, "--capsule", 0, "--pattern", 1, *extra)
    f, brk = spectrum("--distance", 1.0, "--capsule", 0, "--pattern", 1, "--breakup", 1, *extra)
    u, _ = analytic(np.maximum(f, 1), P)            # cone (coil) velocity per amp volt
    spl = lambda h: db(2.83 * u * h / 20e-6)
    band = (f > 60) & (f < 8000)
    s_r, s_b = smooth(f[band], spl(rigid)[band]), smooth(f[band], spl(brk)[band])
    fb = f[band]
    print("SPL at 1 m, 2.83 V, 1/6 octave:")
    for fq in [100, 200, 500, 1000, 1500, 2000, 3000, 4000, 5000, 6000]:
        i = np.argmin(np.abs(fb - fq))
        print(f"   {fq:5d} Hz: rigid {s_r[i]:6.1f}   breakup {s_b[i]:6.1f} dB SPL")
    i1 = (fb > 500) & (fb < 5000)
    print(f"   breakup ripple 500 Hz - 5 kHz: {s_b[i1].max() - s_b[i1].min():.1f} dB peak-to-peak")

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 2, figsize=(13, 4.8))
        ax[0].semilogx(fb, s_r, lw=1.5, label="rigid piston")
        ax[0].semilogx(fb, s_b, lw=1.5, label="with cone breakup")
        ax[0].set_ylim(75, 115)
        ax[0].set_title('12" guitar speaker, 50 l closed box: 2.83 V / 1 m on axis (1/6 oct)')
        ax[0].set_ylabel("dB SPL")
        for off in [0.0, 0.03, 0.06, 0.09, 0.12]:
            f2, h = spectrum("--offset", off, "--distance", 0.025, "--capsule", 0.02, "--pattern", 0.5, "--breakup", 1, *extra)
            u2, _ = analytic(np.maximum(f2, 1), P)
            m = (f2 > 60) & (f2 < 12000)
            ax[1].semilogx(f2[m], smooth(f2[m], db(2.83 * u2[m] * h[m] / 20e-6)), lw=1.3, label=f"{off*100:.0f} cm off centre")
        ax[1].set_title("close mic (cardioid, 2 cm capsule, 2.5 cm), with breakup (1/6 oct)")
        for x in ax:
            x.grid(alpha=0.3, which="both"); x.legend(fontsize=8); x.set_xlabel("Hz")
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
