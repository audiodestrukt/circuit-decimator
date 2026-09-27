#!/usr/bin/env python3
"""Speaker driver in a closed box: exact analytic network vs ngspice (speaker.cir)
vs the engine (build/speaker_render, core/circuit/circuits/Speaker.h).

Checks:
  1. cone velocity / amp volts and input impedance, magnitude and phase, 10 Hz - 20 kHz
  2. closed-box theory: system resonance fc = fs sqrt(1 + Vas/Vb), Q = Qts sqrt(1 + Vas/Vb)
     (lossless box, ideal amp, Le -> 0), fitted from the analytic acceleration response
  3. step response in the time domain (engine vs ngspice transient)

  python3 compare.py [--plot out.png]
"""
import argparse, os, re, shutil, subprocess, tempfile
import numpy as np

os.environ["OMP_NUM_THREADS"] = "1"
HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.join(HERE, "speaker.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "speaker_render")
RHO, C0 = 1.204, 343.0

P = dict(ramp=0.05, re=6.4, le=0.4e-3, l2=0.8e-3, r2=12, bl=12, mms=0.022, cms=1.8e-4, rms=1.84,
         sd=0.053, vb=0.05, qa=20, open=0.0, panel=0.018)


def opening(p):
    """the back opening's air plug: acoustic mass and (constant, circuit) radiation resistance"""
    cab = p["vb"] / (RHO * C0 ** 2)
    o = p.get("open", 0.0)
    mp = min(1e6, RHO * (p.get("panel", 0.018) + 1.7 * np.sqrt(o / np.pi)) / o) if o > 0 else 1e6
    wh = 1 / np.sqrt(mp * cab)
    return mp, RHO * wh ** 2 / (2 * np.pi * C0), wh / (2 * np.pi)


def ts(p):
    ws = 1 / np.sqrt(p["mms"] * p["cms"])
    qes = ws * p["mms"] * p["re"] / p["bl"] ** 2
    qms = ws * p["mms"] / p["rms"]
    vas = RHO * C0 ** 2 * p["sd"] ** 2 * p["cms"]
    return ws / (2 * np.pi), qes, qms, qes * qms / (qes + qms), vas


def analytic(f, p):
    """velocity/V and Z of the network, exactly as drawn (mobility analogy)"""
    w = 2 * np.pi * f
    s = 1j * w
    fs0, _, _, _, vas = ts(p)
    fc = fs0 * np.sqrt(1 + vas / p["vb"])
    cab = p["vb"] / (RHO * C0 ** 2)
    rab = 1 / (2 * np.pi * fc * cab * p["qa"]) if np.isfinite(p["qa"]) else 0.0
    ze = p["re"] + s * p["le"] + (s * p["l2"] * p["r2"]) / (s * p["l2"] + p["r2"])
    y1 = 1 / (s * cab) + rab                       # box air: inductor Cab, conductance R_ab
    mp, rp, _ = opening(p)
    y2 = s * mp + rp                               # back opening: capacitor M_p, conductance R_p
    yq = y1 * y2 / (y1 + y2)                       # in series (mobility), at q
    yu = s * p["mms"] + 1 / (s * p["cms"]) + p["rms"] + p["sd"] ** 2 * yq
    zmot = p["bl"] ** 2 / yu
    i = 1 / (p["ramp"] + ze + zmot)
    u = i * zmot / p["bl"]
    return u, 1 / i   # Z seen by the source (includes the amp's R)


def spice(over=None):
    work = tempfile.mkdtemp(prefix="spk_")
    txt = open(DECK).read()
    for k, v in (over or {}).items():
        txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
        assert n, k
    open(os.path.join(work, "d.cir"), "w").write(txt)
    subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True, check=True)
    ac = np.loadtxt(os.path.join(work, "ac.dat"), skiprows=1)
    st = np.loadtxt(os.path.join(work, "step.dat"), skiprows=1)
    return ac, st


def engine_sweep(freqs, over=None):
    sets = sum((["--set", f"{k}={v}"] for k, v in (over or {}).items()), [])
    r = subprocess.run([TOOL, "sweep", str(freqs[0]), str(freqs[-1]), str(len(freqs)), *sets],
                       capture_output=True, text=True, check=True)
    return np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])


def engine_step():
    out = tempfile.mktemp(suffix=".dat")
    subprocess.run([TOOL, "step", out], check=True, capture_output=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return d


def fit_hp2(f, h):
    """fit |h| to a 2nd-order high-pass a s^2/(s^2 + s wc/Q + wc^2); returns fc, Q"""
    from scipy.optimize import least_squares

    def res(x):
        a, fc, q = x
        s = 1j * f / fc
        m = np.abs(a * s ** 2 / (s ** 2 + s / q + 1))
        return 20 * np.log10(m) - 20 * np.log10(np.abs(h))
    r = least_squares(res, [np.abs(h[-1]), 100, 0.7], bounds=([0, 10, 0.1], [np.inf, 1000, 10]))
    return r.x[1], r.x[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plot")
    a = ap.parse_args()
    fs0, qes, qms, qts, vas = ts(P)
    fc_th = fs0 * np.sqrt(1 + vas / P["vb"])
    q_th = qts * np.sqrt(1 + vas / P["vb"])
    print(f"driver: Fs {fs0:.1f} Hz  Qes {qes:.3f}  Qms {qms:.2f}  Qts {qts:.3f}  Vas {vas*1e3:.1f} l;  box Vb {P['vb']*1e3:.0f} l")

    ac, st = spice()
    f = ac[:, 0]
    u_a, z_a = analytic(f, P)
    hs = ac[:, 1] * np.exp(1j * ac[:, 2])
    zs = ac[:, 3] * np.exp(1j * ac[:, 4])
    db = lambda x: 20 * np.log10(np.abs(x))
    print("\n1. frequency response, 10 Hz - 20 kHz")
    print(f"   ngspice vs analytic: velocity {np.max(np.abs(db(hs) - db(u_a))):.2e} dB max, "
          f"impedance {np.max(np.abs(db(zs) - db(z_a))):.2e} dB max")
    fe = np.geomspace(20, 5000, 25)
    e = engine_sweep(fe)
    u_ae, z_ae = analytic(e[:, 0], P)
    he = e[:, 1] * np.exp(1j * e[:, 2])
    ze = e[:, 3] * np.exp(1j * e[:, 4])
    dph = np.angle(he / u_ae)
    print(f"   engine vs analytic:  velocity {np.max(np.abs(db(he) - db(u_ae))):.2e} dB max, "
          f"phase {np.max(np.abs(dph)) * 180 / np.pi:.3f} deg max;  impedance {np.max(np.abs(db(ze) - db(z_ae))):.2e} dB max  (25 points, 96 kHz)")
    k = np.argmax(np.abs(z_a))
    print(f"   impedance peak: {np.abs(z_a[k]):.1f} ohm at {f[k]:.1f} Hz (analytic/ngspice); "
          f"engine sweep max {np.max(e[:, 3]):.1f} ohm")

    print("\n2. closed-box theory (lossless box, ideal amp, Le -> 0)")
    ideal = dict(P, ramp=0.0, le=1e-12, l2=1e-12, qa=np.inf)
    fg = np.geomspace(10, 2000, 400)
    u_i, _ = analytic(fg, ideal)
    fc_fit, q_fit = fit_hp2(fg, 1j * 2 * np.pi * fg * u_i)   # acceleration ~ far-field pressure
    print(f"   theory fc {fc_th:.2f} Hz, Qtc {q_th:.3f};  fitted from the network: fc {fc_fit:.2f} Hz, Q {q_fit:.3f}")
    u_r, _ = analytic(fg, P)
    fc_r, q_r = fit_hp2(fg[fg < 400], (1j * 2 * np.pi * fg * u_r)[fg < 400])
    print(f"   with the real amp, coil inductance, Qa {P['qa']}: fc {fc_r:.2f} Hz, Q {q_r:.3f}")

    print("\n3. step response (1 V at 1 ms)")
    es = engine_step()
    t = es[:, 0]
    us = np.interp(t, st[:, 0], st[:, 1])
    err = np.sqrt(np.mean((es[:, 1] - us) ** 2)) / np.sqrt(np.mean(us ** 2))
    print(f"   cone velocity, engine vs ngspice: rms err/rms {err:.2e}, peak {np.max(np.abs(us))*1e3:.2f} mm/s")

    print("\n4. back opening (open back): engine and ngspice vs the exact network")
    for area in [0.002, 0.01, 0.05, 0.2]:
        q = dict(P, open=area)
        _, _, fh = opening(q)
        ac, _ = spice({"open": area})
        f2 = ac[:, 0]
        u2, z2 = analytic(f2, q)
        hs2 = ac[:, 1] * np.exp(1j * ac[:, 2])
        fe2 = np.geomspace(20, 5000, 20)
        e2 = engine_sweep(fe2, {"open": area})
        u2e, _ = analytic(e2[:, 0], q)
        he2 = e2[:, 1] * np.exp(1j * e2[:, 2])
        print(f"   {area*1e4:6.0f} cm^2 (Helmholtz {fh:6.1f} Hz): ngspice {np.max(np.abs(db(hs2) - db(u2))):.1e} dB, "
              f"engine {np.max(np.abs(db(he2) - db(u2e))):.3f} dB max vs exact")
    # a small opening: the impedance gets the vented-box double peak with its dip at the Helmholtz frequency
    q = dict(P, open=0.002)
    _, _, fh = opening(q)
    fg = np.geomspace(10, 400, 4000)
    _, zq = analytic(fg, q)
    band = (fg > fh / 2) & (fg < fh * 2)
    fmin = fg[band][np.argmin(np.abs(zq[band]))]
    print(f"   20 cm^2: impedance dip at {fmin:.1f} Hz, Helmholtz formula {fh:.1f} Hz")

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        spl = lambda u, ff: 20 * np.log10(RHO * P["sd"] * 2 * np.pi * ff * np.abs(u) * 2.83 / (2 * np.pi * 1.0) / 20e-6)
        fig, ax = plt.subplots(1, 2, figsize=(12, 4.5))
        ax[0].semilogx(f, spl(u_a, f), lw=3, alpha=0.4, label="analytic")
        ax[0].semilogx(f, spl(hs, f), lw=1.2, label="ngspice")
        ax[0].semilogx(e[:, 0], spl(he, e[:, 0]), "o", ms=4, label="engine")
        ax[0].set_title("piston SPL, 2.83 V @ 1 m (half space, below breakup)")
        ax[0].set_xlabel("Hz"); ax[0].set_ylabel("dB SPL"); ax[0].set_ylim(60, 110)
        ax[1].semilogx(f, np.abs(z_a), lw=3, alpha=0.4, label="analytic")
        ax[1].semilogx(f, np.abs(zs), lw=1.2, label="ngspice")
        ax[1].semilogx(e[:, 0], e[:, 3], "o", ms=4, label="engine")
        ax[1].set_title("input impedance |Z|"); ax[1].set_xlabel("Hz"); ax[1].set_ylabel("ohm")
        for x in ax:
            x.grid(alpha=0.3, which="both"); x.legend()
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
