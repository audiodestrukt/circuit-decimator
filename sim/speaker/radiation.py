#!/usr/bin/env python3
"""Cone-to-mic radiation (core/acoustic/Radiation.h via build/radiation_check)
against analytic results for a rigid piston in an infinite baffle:

  1. on-axis pressure, any distance (exact Rayleigh result):
       |p/u| = 2 rho c |sin(k/2 (sqrt(r^2 + a^2) - r))|
  2. far-field directivity: 2 J1(ka sin t) / (ka sin t)
  3. proximity effect of a pressure-gradient (figure-8) mic near a small source:
       |gradient / pressure| = |1 + 1/(jkr)|
  4. capsule size: a small capsule converges to the point mic

then renders what the model says about a real 12" cone (recessed, with a dust
cap) and a 2 cm cardioid capsule at 2.5 cm, moved from the dust cap to the
edge -- rigid piston only, so this is the geometry's share of the mic-position
effect before breakup is modelled.

  python3 radiation.py [--plot out.png]
"""
import argparse, os, subprocess
import numpy as np
from scipy.special import j1

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "build", "radiation_check")
RHO, C0 = 1.204, 343.0
FLAT = ["--depth", "0", "--caph", "0"]


def spectrum(*args, fs=48000, n=4096):
    r = subprocess.run([TOOL, "spectrum", "--fs", str(fs), "--n", str(n), *map(str, args)],
                       capture_output=True, text=True, check=True)
    d = np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])
    return d[:, 0], d[:, 1] + 1j * d[:, 2]


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plot")
    a = ap.parse_args()
    rad = 0.13

    print("1. on-axis, flat piston a = 13 cm, omni point mic (dB error where |p| is within 20 dB of its peak)")
    for r in [0.025, 0.1, 0.3, 1.0]:
        f, h = spectrum(*FLAT, "--capsule", 0, "--pattern", 1, "--distance", r)
        k = 2 * np.pi * f / C0
        ref = 2 * RHO * C0 * np.abs(np.sin(k / 2 * (np.sqrt(r * r + rad * rad) - r)))
        band = (f > 50) & (f < 16000) & (db(ref) > db(ref).max() - 20)
        err = np.abs(db(h) - db(ref))[band]
        print(f"   r = {r*100:5.1f} cm: max {err.max():.3f} dB, median {np.median(err):.4f} dB")

    print("\n2. far-field directivity at 4 m, |H(t)|/|H(0)| vs 2 J1(x)/x")
    f0, h0 = spectrum(*FLAT, "--capsule", 0, "--pattern", 1, "--distance", 4.0, n=8192)
    rows = []
    for deg in [15, 30, 45, 60, 75]:
        t = np.radians(deg)
        f, h = spectrum(*FLAT, "--capsule", 0, "--pattern", 1, "--distance", 4 * np.cos(t),
                        "--offset", 4 * np.sin(t), "--angle", deg, n=8192)
        for fq in [500, 1000, 2000, 4000]:
            i = np.argmin(np.abs(f - fq))
            x = 2 * np.pi * f[i] / C0 * rad * np.sin(t)
            ref = abs(2 * j1(x) / x)
            rows.append((deg, fq, db(h[i] / h0[i]), db(ref)))
    err = np.array([abs(r[2] - r[3]) for r in rows if r[3] > -25])
    print(f"   {len(rows)} angle/frequency points: max {err.max():.3f} dB (where the lobe is above -25 dB)")
    for deg, fq, m, r in [x for x in rows if x[0] in (30, 60)]:
        print(f"   {deg:2d} deg @ {fq} Hz: model {m:6.2f} dB, theory {r:6.2f} dB")

    print("\n3. proximity: figure-8 / omni for a 1 cm piston on axis, vs |1 + 1/(jkr)|")
    print("   (the formula is for a point source: at 2 cm the 1 cm piston is not one)")
    for r in [0.02, 0.05, 0.2]:
        f, hp = spectrum(*FLAT, "--radius", 0.01, "--dustcap", 0.002, "--capsule", 0, "--pattern", 1, "--distance", r)
        f, hg = spectrum(*FLAT, "--radius", 0.01, "--dustcap", 0.002, "--capsule", 0, "--pattern", 0, "--distance", r)
        band = (f > 20) & (f < 5000)
        f, hg, hp = f[1:], hg[1:], hp[1:]
        band = band[1:]
        k = 2 * np.pi * f / C0
        ref = np.abs(1 + 1 / (1j * k * r))
        err = np.abs(db(hg / hp) - db(ref))[band]
        i100 = np.argmin(np.abs(f - 100))
        print(f"   r = {r*100:4.0f} cm: max {err.max():.3f} dB;  bass boost at 100 Hz {db(hg[i100]/hp[i100]):.1f} dB (theory {db(ref[i100]):.1f})")

    print("\n4. capsule size -> point mic (12\" cone, 2.5 cm, 5 cm off axis)")
    f, hpt = spectrum("--capsule", 0, "--offset", 0.05)
    for cap in [0.002, 0.01, 0.025, 0.04]:
        f, hc = spectrum("--capsule", cap, "--offset", 0.05)
        i8k = np.argmin(np.abs(f - 8000))
        band = (f > 50) & (f < 12000)
        print(f"   capsule {cap*1000:4.0f} mm: vs point {np.max(np.abs(db(hc) - db(hpt))[band]):5.2f} dB max below 12 kHz, "
              f"{db(hc[i8k]) - db(hpt[i8k]):+6.2f} dB at 8 kHz")

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 2, figsize=(13, 4.8))
        for r in [0.025, 0.3]:
            f, h = spectrum(*FLAT, "--capsule", 0, "--pattern", 1, "--distance", r)
            k = 2 * np.pi * f / C0
            ref = 2 * RHO * C0 * np.abs(np.sin(k / 2 * (np.sqrt(r * r + rad * rad) - r)))
            ax[0].semilogx(f[1:], db(ref[1:]), lw=3, alpha=0.4, label=f"theory {r*100:.1f} cm")
            ax[0].semilogx(f[1:], db(h[1:]), lw=1, label=f"model {r*100:.1f} cm")
        ax[0].set_xlim(50, 20000); ax[0].set_ylim(0, 70)
        ax[0].set_title("flat piston, on axis: pressure per unit cone velocity")
        ax[0].set_ylabel("dB re 1 Pa/(m/s)")
        for off in [0.0, 0.03, 0.06, 0.09, 0.12]:
            f, h = spectrum("--offset", off, "--distance", 0.025, "--capsule", 0.02, "--pattern", 0.5)
            ax[1].semilogx(f[1:], db(h[1:]), lw=1.3, label=f"{off*100:.0f} cm off centre")
        ax[1].set_xlim(50, 20000); ax[1].set_ylim(10, 70)
        ax[1].set_title('12" cone (rigid), cardioid 2 cm capsule at 2.5 cm')
        for x in ax:
            x.grid(alpha=0.3, which="both"); x.legend(fontsize=8); x.set_xlabel("Hz")
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
