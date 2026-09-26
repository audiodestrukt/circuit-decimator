#!/usr/bin/env python3
"""Calibrate the cab model's cone against a published speaker: the Eminence
Legend 1258 (sim/speaker/reference/).

The lumped driver comes straight from the datasheet's Thiele-Small set; the
cone's frequency-dependent load on the motor (Coupling.h) corrects the coil
velocity above breakup. The
cone's physical parameters (paper stiffness, density, thickness, loss;
depth and profile; surround radial stiffness and damping; dust cap mass and
size) are fitted so the model's on-axis SPL at 1 m, in an infinite baffle,
2.83 V, 1/6-octave smoothed (the datasheet's conditions), matches the
published curve from 80 Hz to 6 kHz. All bounds are realistic for paper
cones, and a penalty keeps the moving parts' mass consistent with the
datasheet's Mms.

  python3 calibrate.py                 # evaluate the current defaults
  python3 calibrate.py --fit [--iters N] [--plot out.png]
"""
import argparse, json, os, subprocess, sys
import numpy as np
from scipy.optimize import differential_evolution

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from compare import analytic  # noqa: E402

TOOL = os.path.join(HERE, "..", "..", "build", "radiation_check")
REF = os.path.join(HERE, "reference")
RHO = 1.204

# Legend 1258 datasheet (reference/README.md); infinite baffle: no box
DRIVER = dict(ramp=0.05, re=7.44, le=0.7e-3, l2=1e-9, r2=1e6, bl=10.9, mms=0.032, cms=8.96e-5, rms=3.07,
              sd=0.05067, vb=1e3, qa=np.inf)
# the coil's lossy inductance, fitted to the datasheet impedance (--electrical), once it exists;
# at import, so the optimiser's worker processes see it too
COIL_FIT = os.path.join(REF, "legend1258_coil.json")
if os.path.exists(COIL_FIT):
    DRIVER.update(json.load(open(COIL_FIT)))
RADIUS, COIL = np.sqrt(0.05067 / np.pi), 0.019
MMS_PARTS = 0.032 - 2 * 8 * RHO * RADIUS ** 3 / 3 - 0.005   # Mms minus both sides' air load and ~5 g coil + former

# name, lower, upper, log-scale, radiation_check flag
PARAMS = [   # bounds: realistic for pressed paper cones
    ("E", 1.0e9, 5.0e9, True, "--E"),
    ("rho", 400, 800, False, "--rhoc"),
    ("h", 0.3e-3, 0.7e-3, False, "--h"),             # at the edge
    ("taper", 1.0, 3.0, False, "--taper"),           # neck / edge thickness
    ("aniso", 0.2, 1.0, False, "--aniso"),           # E around / E along the slope
    ("ribs", 1.0, 20.0, True, "--ribs"),             # extra bending stiffness along the slope
    ("eta", 0.03, 0.3, True, "--eta"),
    ("depth", 0.03, 0.06, False, "--depth"),
    ("curve", 0.0, 1.5, False, "--curve"),
    ("skr", 1e4, 1e8, True, "--skr"),
    ("sr", 0.1, 3.0, True, "--sr"),                  # surround damping: part of Rms (3.07), can't exceed it
    ("capm", 0.1e-3, 1.5e-3, True, "--capm"),
    ("dustcap", 0.035, 0.06, False, "--dustcap"),
]
DEFAULTS = dict(E=4.0e9, rho=500, h=0.6e-3, taper=1.0, aniso=1.0, ribs=1.0, eta=0.1, depth=0.045, curve=0.0,
                skr=1e5, sr=1.0, capm=0.3e-3, dustcap=0.05)

ref = np.loadtxt(os.path.join(REF, "legend1258_spl.csv"), delimiter=",", skiprows=1)
ref = ref[np.argsort(ref[:, 0])]
FGRID = np.geomspace(80, 6000, 120)
REF_DB = np.interp(np.log(FGRID), np.log(ref[:, 0]), ref[:, 1])


def cone_mass(v):
    t = np.linspace(0, 1, 200)
    r = COIL + t * (RADIUS - COIL)
    z = -v["depth"] * (1 - t) * (1 + v["curve"] * t)
    tm = 0.5 * (t[1:] + t[:-1])
    h = v["h"] * (v["taper"] + (1 - v["taper"]) * tm)
    dA = np.hypot(np.diff(r), np.diff(z)) * 2 * np.pi * 0.5 * (r[1:] + r[:-1])
    return v["rho"] * np.sum(h * dA)


def smooth(f, y, frac=1 / 6):
    p = 10 ** (y / 10)
    return np.array([10 * np.log10(np.mean(p[(f > fc * 2 ** (-frac / 2)) & (f < fc * 2 ** (frac / 2))])) for fc in f])


def model_spl(v, fs=24000, n=2048, fmax=8000):
    args = [TOOL, "spectrum", "--fs", str(fs), "--n", str(n), "--fmax", str(fmax), "--breakup", "1",
            "--distance", "1.0", "--capsule", "0", "--pattern", "1", "--radius", str(RADIUS), "--coil", str(COIL),
            "--sk", str(0.4 / DRIVER["cms"]),
            # the cone loads the motor (Coupling.h): the datasheet driver, infinite baffle
            "--couple", "1", "--vb", "1e3", "--re", str(DRIVER["re"]), "--le", str(DRIVER["le"]),
            "--l2", str(DRIVER["l2"]), "--r2", str(DRIVER["r2"]), "--bl", str(DRIVER["bl"]),
            "--mms", str(DRIVER["mms"]), "--cms", str(DRIVER["cms"]), "--rms", str(DRIVER["rms"]),
            "--sd", str(DRIVER["sd"])]
    for name, _, _, _, flag in PARAMS:
        args += [flag, str(v[name])]
    r = subprocess.run(args, capture_output=True, text=True, check=True)
    d = np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])
    f, h = d[1:, 0], d[1:, 1] + 1j * d[1:, 2]
    u, _ = analytic(f, DRIVER)
    spl = 20 * np.log10(np.maximum(np.abs(2.83 * u * h / 20e-6), 1e-9))
    return f, smooth(f, spl)


def model_impedance(v, fs=24000, n=2048, fmax=8000):
    args = [TOOL, "impedance", "--fs", str(fs), "--n", str(n), "--fmax", str(fmax), "--breakup", "1",
            "--radius", str(RADIUS), "--coil", str(COIL), "--sk", str(0.4 / DRIVER["cms"]), "--vb", "1e3"]
    for key in ["re", "le", "l2", "r2", "bl", "mms", "cms", "rms", "sd"]:
        args += ["--" + key, str(DRIVER[key])]
    for name, _, _, _, flag in PARAMS:
        args += [flag, str(v[name])]
    r = subprocess.run(args, capture_output=True, text=True, check=True)
    d = np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])
    return d[:, 0], np.abs(d[:, 1] + 1j * d[:, 2])


def fit_electrical(v):
    """voice coil Le, L2 || R2 to the datasheet impedance, 300 Hz - 8 kHz (with the cone as fitted)"""
    from scipy.optimize import least_squares
    zr = np.loadtxt(os.path.join(REF, "legend1258_impedance.csv"), delimiter=",", skiprows=1)
    zr = zr[np.argsort(zr[:, 0])]
    zg = np.geomspace(300, 8000, 60)
    target = np.interp(np.log(zg), np.log(zr[:, 0]), zr[:, 1])

    def res(x):
        DRIVER.update(le=np.exp(x[0]), l2=np.exp(x[1]), r2=np.exp(x[2]))
        fz, zm = model_impedance(v)
        return 20 * np.log10(np.interp(np.log(zg), np.log(fz), zm) / target)
    r = least_squares(res, np.log([0.7e-3, 1e-3, 10.0]), bounds=(np.log([0.05e-3, 1e-5, 0.5]), np.log([3e-3, 20e-3, 200])))
    res(r.x)
    return dict(le=DRIVER["le"], l2=DRIVER["l2"], r2=DRIVER["r2"])


def error(v):
    f, s = model_spl(v)
    m = np.interp(np.log(FGRID), np.log(f), s)
    rms = np.sqrt(np.mean((m - REF_DB) ** 2))
    mass = cone_mass(v) + v["capm"] + 2e-3          # + surround
    penalty = 10 * max(0.0, abs(mass - MMS_PARTS) / MMS_PARTS - 0.25)   # allow +-25 %
    return rms + penalty


def unpack(x):
    v = {}
    for (name, lo, hi, log, _), xi in zip(PARAMS, x):
        v[name] = float(np.exp(xi)) if log else float(xi)
    return v


def objective(x):
    try:
        return error(unpack(x))
    except Exception:
        return 1e3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fit", action="store_true")
    ap.add_argument("--iters", type=int, default=25)
    ap.add_argument("--plot")
    ap.add_argument("--electrical", action="store_true", help="fit the coil's Le, L2 || R2 to the impedance, then refit the cone")
    a = ap.parse_args()
    ep = COIL_FIT
    if os.path.exists(ep) and not a.electrical:
        print(f"coil (fitted to impedance): Le {DRIVER['le']*1e3:.3f} mH, L2 {DRIVER['l2']*1e3:.3f} mH, R2 {DRIVER['r2']:.2f} ohm")
    print(f"moving parts other than coil + air load: {MMS_PARTS*1e3:.1f} g (target for cone + dust cap + surround)")
    v = dict(DEFAULTS)
    print(f"defaults: rms error {error(v):.2f} dB, cone mass {cone_mass(v)*1e3:.1f} g")
    if a.electrical:
        cone = json.load(open(os.path.join(REF, "legend1258_fit.json")))
        coil = fit_electrical(cone)
        json.dump(coil, open(ep, "w"), indent=1)
        print(f"coil fitted to impedance: Le {coil['le']*1e3:.3f} mH, L2 {coil['l2']*1e3:.3f} mH, R2 {coil['r2']:.2f} ohm")
        a.fit = True
    if a.fit:
        bounds = [(np.log(lo), np.log(hi)) if log else (lo, hi) for _, lo, hi, log, _ in PARAMS]
        best = differential_evolution(objective, bounds, maxiter=a.iters, popsize=12, seed=3, workers=-1,
                                      updating="deferred", polish=False, tol=1e-6,
                                      callback=lambda xk, convergence=None: print(f"   best {objective(xk):.2f} dB", flush=True))
        v = unpack(best.x)
        print(f"fitted: rms error {best.fun:.2f} dB, cone mass {cone_mass(v)*1e3:.1f} g")
        for name, lo, hi, *_ in PARAMS:
            edge = "  <- at bound" if min(abs(v[name] - lo), abs(v[name] - hi)) < 0.02 * (hi - lo) else ""
            print(f"   {name:8s} {v[name]:.4g}   (range {lo:.3g} .. {hi:.3g}){edge}")
        json.dump(v, open(os.path.join(REF, "legend1258_fit.json"), "w"), indent=1)
    elif os.path.exists(os.path.join(REF, "legend1258_fit.json")):
        v = json.load(open(os.path.join(REF, "legend1258_fit.json")))
        print(f"saved fit: rms error {error(v):.2f} dB")
    # impedance: the coil's inductance is fitted to it (--electrical); the resonance and the
    # breakup bumps on it are predictions
    zr = np.loadtxt(os.path.join(REF, "legend1258_impedance.csv"), delimiter=",", skiprows=1)
    zr = zr[np.argsort(zr[:, 0])]
    fz, zm = model_impedance(v)
    zg = np.geomspace(40, 6000, 100)
    zm_i = np.interp(np.log(zg), np.log(fz), zm)
    zr_i = np.interp(np.log(zg), np.log(zr[:, 0]), zr[:, 1])
    print(f"impedance: rms {np.sqrt(np.mean((20*np.log10(zm_i/zr_i))**2)):.2f} dB, "
          f"peak {zm.max():.1f} ohm at {fz[np.argmax(zm)]:.0f} Hz (datasheet {zr[:,1][zr[:,0]<1000].max():.1f} ohm at "
          f"{zr[:,0][np.argmax(zr[:,1][zr[:,0]<1000])]:.0f} Hz)")
    for fq in [200, 500, 1000, 2000, 3000, 5000]:
        print(f"   {fq:5d} Hz: model {np.interp(fq, fz, zm):5.1f} ohm, datasheet {np.interp(fq, zr[:,0], zr[:,1]):5.1f} ohm")
    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, (ax, az) = plt.subplots(1, 2, figsize=(14, 4.8))
        az.semilogx(zr[:, 0], zr[:, 1], lw=3, alpha=0.45, color="C3", label="Legend 1258 datasheet")
        az.semilogx(fz, zm, lw=1.5, color="C0", label="model (coil inductance fitted; bumps predicted)")
        az.set_xlim(20, 10000); az.set_ylim(0, 60); az.set_xlabel("Hz"); az.set_ylabel("ohm")
        az.set_title("input impedance")
        az.grid(alpha=0.3, which="both"); az.legend()
        ax.semilogx(ref[:, 0], ref[:, 1], lw=3, alpha=0.45, color="C3", label="Legend 1258 datasheet")
        f, s = model_spl(DEFAULTS)
        ax.semilogx(f, s, lw=1, color="C7", label="model, provisional defaults")
        f, s = model_spl(v)
        ax.semilogx(f, s, lw=1.5, color="C0", label="model, fitted cone")
        ax.set_xlim(50, 10000); ax.set_ylim(70, 115)
        ax.set_xlabel("Hz"); ax.set_ylabel("dB SPL, 2.83 V / 1 m")
        ax.set_title("on-axis response, infinite baffle, 1/6 octave")
        ax.grid(alpha=0.3, which="both"); ax.legend()
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
