#!/usr/bin/env python3
"""Cone breakup model (core/acoustic/Cone.h via build/cone_check) checks.

  1. flat limit: a driven annular plate (inner edge clamped to the moving coil,
     outer edge free) against the exact Bessel-function solution
       w = A J0(br) + B Y0(br) + C I0(br) + D K0(br),  b^4 = rho h w^2 / D(1 + j eta)
  1b. flat limit, in-plane: the annulus driven radially at the hub (membrane
     terms) against the exact solution u = A J1(kr) + B Y1(kr),
     k^2 = rho (1 - v^2) w^2 / E(1 + j eta), free edge: du/dr + v u/r = 0
  2. rigid limit: a very stiff cone moves as a piston (T -> 1)
  3. mesh convergence on the real cone
  then plots T(f) across the real cone.

  python3 cone.py [--plot out.png]
"""
import argparse, os, subprocess
import numpy as np
from scipy.special import jv, yv, iv, kv

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "build", "cone_check")


MAT = ["E", 2.0e9, "rho", 500, "h", 0.4e-3, "nu", 0.3, "eta", 0.04]   # the exact checks' material


def run(*args):
    r = subprocess.run([TOOL, *map(str, MAT), *map(str, args)], capture_output=True, text=True, check=True)
    d = np.array([[float(x) for x in ln.split()] for ln in r.stdout.strip().splitlines()])
    f = d[:, 0]
    T = d[:, 1:11:2] + 1j * d[:, 2:12:2]
    cap = d[:, 11] + 1j * d[:, 12]
    return f, T, cap, r.stderr


def plate_exact(f, a, b, h, rho, E, nu, eta):
    """outer-edge / inner-edge displacement of an annular plate, inner clamped and driven, outer free"""
    D = E * h ** 3 / (12 * (1 - nu ** 2)) * (1 + 1j * eta)
    out = []
    for fq in f:
        w = 2 * np.pi * fq
        be = (rho * h * w ** 2 / D) ** 0.25
        def funcs(r):
            x = be * r
            J0, J1, Y0, Y1 = jv(0, x), jv(1, x), yv(0, x), yv(1, x)
            I0, I1, K0, K1 = iv(0, x), iv(1, x), kv(0, x), kv(1, x)
            val = np.array([J0, Y0, I0, K0])
            d1 = np.array([-be * J1, -be * Y1, be * I1, -be * K1])
            d2 = np.array([-be ** 2 * J0 + be / r * J1, -be ** 2 * Y0 + be / r * Y1,
                           be ** 2 * I0 - be / r * I1, be ** 2 * K0 + be / r * K1])
            # d/dr of the Laplacian: Lap = -b^2 (A J0 + B Y0) + b^2 (C I0 + D K0)
            d3 = np.array([-be ** 2 * (-be * J1), -be ** 2 * (-be * Y1), be ** 2 * (be * I1), be ** 2 * (-be * K1)])
            return val, d1, d2, d3
        vb, d1b, _, _ = funcs(b)
        _, d1a, d2a, d3a = funcs(a)
        M = np.array([vb, d1b, d2a + nu * d1a / a, d3a])
        rhs = np.array([1, 0, 0, 0], dtype=complex)
        coef = np.linalg.solve(M, rhs)
        va, _, _, _ = funcs(a)
        out.append(va @ coef)
    return np.array(out)


def inplane_exact(f, a, b, rho, E, nu, eta):
    """outer-edge radial displacement of an annulus whose hub moves radially by 1, outer edge free"""
    Ec = E * (1 + 1j * eta)
    out = []
    for fq in f:
        k = np.sqrt(rho * (1 - nu ** 2) * (2 * np.pi * fq) ** 2 / Ec)
        def u(r):
            return np.array([jv(1, k * r), yv(1, k * r)])
        def dur(r):   # d/dr J1(kr) = k (J0 - J1/(kr))
            x = k * r
            return np.array([k * (jv(0, x) - jv(1, x) / x), k * (yv(0, x) - yv(1, x) / x)])
        M = np.array([u(b), dur(a) + nu * u(a) / a])
        c = np.linalg.solve(M, np.array([1, 0], dtype=complex))
        out.append(u(a) @ c)
    return np.array(out)


def db(x):
    return 20 * np.log10(np.maximum(np.abs(x), 1e-12))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plot")
    a = ap.parse_args()
    E, rho, h, nu, eta = 2.0e9, 500, 0.4e-3, 0.3, 0.04
    print("1. flat limit: driven annular plate (b = 22 mm, a = 130 mm), outer/inner displacement vs exact")
    for ne in [24, 48, 96]:
        f, T, _, _ = run("--flat", "--elements", ne, "--f1", 20, "--f2", 20000, "--points", 300)
        ex = plate_exact(f, 0.13, 0.022, h, rho, E, nu, eta)
        err = np.abs(db(T[:, 4]) - db(ex))
        band = db(ex) > -30
        print(f"   {ne:3d} elements: max {err[band].max():.3f} dB (where > -30 dB), "
              f"below 5 kHz {err[band & (f < 5000)].max():.4f} dB")
    print("\n1b. flat limit, in-plane (membrane): hub driven radially, edge radial displacement vs exact")
    r = subprocess.run([TOOL, *map(str, MAT), "--flat", "--radial", "--f1", "100", "--f2", "20000", "--points", "300"],
                       capture_output=True, text=True, check=True).stdout
    d = np.array([[float(x) for x in ln.split()] for ln in r.strip().splitlines()])
    ex = inplane_exact(d[:, 0], 0.13, 0.022, rho, E, nu, eta)
    fe = d[:, 1] + 1j * d[:, 2]
    band = db(ex) > -30
    print(f"   96 elements: max {np.max(np.abs(db(fe) - db(ex))[band]):.3f} dB, first in-plane resonance "
          f"{d[np.argmax(np.abs(ex)), 0]:.0f} Hz (exact) / {d[np.argmax(np.abs(fe)), 0]:.0f} Hz (model)")
    print("\n2. rigid limit: max |T - 1| over 20 Hz - 20 kHz shrinks as 1/E (a stiffer cone's modes move up)")
    for Ex in [1e12, 1e14, 1e16]:
        f, T, cap, _ = run("E", Ex, "--points", 60)
        print(f"   E = {Ex:.0e}: {np.max(np.abs(T - 1)):.2e} (cone), {np.max(np.abs(cap - 1)):.2e} (dust cap)")
    print("\n3. mesh convergence, real cone: mid-cone and edge T vs 384 elements (default 96)")
    f, Tref, _, _ = run("--elements", 384, "--points", 200)
    for ne in [48, 96, 192]:
        f, T, _, _ = run("--elements", ne, "--points", 200)
        print(f"   {ne:3d} elements: max {np.max(np.abs(db(T[:, 2]) - db(Tref[:, 2]))[f < 10000]):.3f} dB mid-cone, "
              f"{np.max(np.abs(db(T[:, 4]) - db(Tref[:, 4]))[f < 10000]):.3f} dB edge (below 10 kHz)")

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 2, figsize=(13, 4.8))
        f, T, _, _ = run("--flat", "--elements", 96, "--points", 400)
        ex = plate_exact(f, 0.13, 0.022, h, rho, E, nu, eta)
        ax[0].semilogx(f, db(ex), lw=3, alpha=0.4, label="exact (Bessel)")
        ax[0].semilogx(f, db(T[:, 4]), lw=1, label="shell FE, 96 elements")
        ax[0].set_title("flat limit: annular plate, free edge / driven hub")
        r = subprocess.run([TOOL, "--points", "400"], capture_output=True, text=True, check=True).stdout
        d = np.array([[float(x) for x in ln.split()] for ln in r.strip().splitlines()])
        f, T, cap = d[:, 0], d[:, 1:11:2] + 1j * d[:, 2:12:2], d[:, 11] + 1j * d[:, 12]
        labels = ["neck", "1/4", "1/2", "3/4", "edge"]
        for k in range(5):
            ax[1].semilogx(f, db(T[:, k]), lw=1.2, label=labels[k])
        ax[1].semilogx(f, db(cap), "k--", lw=1, label="dust cap")
        ax[1].set_title('12" paper cone (default material): cone motion / coil motion')
        for x in ax:
            x.grid(alpha=0.3, which="both"); x.legend(fontsize=8); x.set_xlabel("Hz"); x.set_ylabel("dB")
            x.set_xlim(20, 20000)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
