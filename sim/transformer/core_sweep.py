#!/usr/bin/env python3
"""Run the nonlinear-core transformer deck (xfmr_core.cir) through ngspice.

  python3 core_sweep.py                 # THD vs level at several frequencies + B-H loops
  python3 core_sweep.py --one 2 20      # single run: 2 V peak at 20 Hz, print THD
  python3 core_sweep.py --compare       # ngspice vs the realtime C++ model (build/xfmr_render)

Signature of a real transformer: distortion rises as the level goes up and as
the frequency goes down (flux ~ V / f), mostly odd harmonics, plus low-level
hysteresis distortion that does not vanish at small signals.
"""
import argparse, os, re, subprocess, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np

# ngspice's OpenMP threading makes runs non-deterministic (different timestep
# sequences, occasionally a wrong trajectory), worst when many run in parallel.
os.environ["OMP_NUM_THREADS"] = "1"

HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.join(HERE, "xfmr_core.cir")
XFMR = os.path.join(HERE, "..", "..", "build", "xfmr_render")


def deck(amp, freq, cycles=12, overrides=None):
    txt = open(DECK).read()
    for k, v in {"amp": amp, "freq": freq, "cycles": cycles, **(overrides or {})}.items():
        txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
        if not n:
            raise SystemExit(f"unknown param {k}")
    dt = 1 / (freq * 400)
    txt = re.sub(r"(?m)^tran .*", f"tran {dt:.6g} {cycles / freq:.6g} 0 {dt:.6g}", txt)
    return txt


def simulate(args):
    amp, freq, overrides = args
    work = tempfile.mkdtemp(prefix="xf_")
    with open(os.path.join(work, "d.cir"), "w") as f:
        f.write(deck(amp, freq, overrides=overrides))
    r = subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True, text=True)
    path = os.path.join(work, "core.dat")
    if not os.path.exists(path):
        return amp, freq, None, r.stdout[-1500:] + r.stderr[-1500:]
    return amp, freq, np.loadtxt(path, skiprows=1), None


def simulate_cpp(args):
    """Same drive through the realtime model (core/circuit/Transformer.h) at 192 kHz."""
    amp, freq, _ = args
    out = tempfile.mktemp(suffix=".dat")
    subprocess.run([XFMR, "sine", str(amp), str(freq), "12", out], capture_output=True, check=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return amp, freq, d, None


def thd(t, y, freq, cycles_used=4, harmonics=9):
    """THD (%) of the last whole cycles, resampled uniformly, and the odd/even split."""
    T = 1 / freq
    t1 = t[-1]
    t0 = t1 - cycles_used * T
    n = 256 * cycles_used
    grid = t0 + np.arange(n) * (cycles_used * T / n)
    yy = np.interp(grid, t, y)
    X = np.abs(np.fft.rfft(yy - yy.mean())) / n
    h = [X[cycles_used * k] for k in range(1, harmonics + 1)]
    fund = h[0]
    odd = np.sqrt(sum(v * v for v in h[2::2]))
    even = np.sqrt(sum(v * v for v in h[1::2]))
    return 100 * np.sqrt(odd**2 + even**2) / fund, 100 * odd / fund, 100 * even / fund, 2 * fund


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--one", nargs=2, type=float, metavar=("AMP", "FREQ"))
    ap.add_argument("--compare", action="store_true", help="also run the realtime C++ model")
    a = ap.parse_args()
    if a.one:
        amp, freq, d, err = simulate((a.one[0], a.one[1], None))
        if d is None:
            raise SystemExit(err)
        total, odd, even, fund = thd(d[:, 0], d[:, 1], freq)
        print(f"{amp} V @ {freq} Hz: out {fund:.3f} V peak, THD {total:.3f}% (odd {odd:.3f}, even {even:.3f}), "
              f"B peak {np.abs(d[:, 2]).max():.3f} T")
        return

    levels = np.array([0.03, 0.1, 0.3, 1, 2, 4])       # V peak at the mic source
    freqs = [20, 50, 100, 1000]
    jobs = [(amp, f, None) for f in freqs for amp in levels]
    # ngspice one run at a time: concurrent ngspice processes on hysteresis decks
    # occasionally take a wrong trajectory (see ../tubepre/README.md)
    res = [simulate(j) for j in jobs]
    with ProcessPoolExecutor() as ex:
        cpp = list(ex.map(simulate_cpp, jobs)) if a.compare else []
    ctable = {(amp, f): (thd(d[:, 0], d[:, 1], f), d) for amp, f, d, _ in cpp}
    table = {}
    for amp, freq, d, err in res:
        if d is None:
            print(f"FAILED {amp} V {freq} Hz\n{err}")
            continue
        table[(amp, freq)] = (thd(d[:, 0], d[:, 1], freq), d)

    print(f"THD % at the output (rows: source V peak; dBu = 20log10(Vrms/0.775))")
    print(f"{'V pk':>6s} {'dBu':>6s} " + "".join(f"{f:>10d} Hz" for f in freqs))
    for amp in levels:
        dbu = 20 * np.log10(amp / np.sqrt(2) / 0.775)
        row = "".join(f"{table[(amp, f)][0][0]:13.3f}" if (amp, f) in table else f"{'--':>13s}" for f in freqs)
        print(f"{amp:6.2f} {dbu:6.1f} {row}")
        if ctable:
            crow = "".join(f"{ctable[(amp, f)][0][0]:13.3f}" for f in freqs)
            print(f"{'C++':>13s} {crow}")
    if ctable:
        errs = [abs(ctable[k][0][0] - table[k][0][0]) / max(table[k][0][0], 1e-3) for k in table]
        gains = [abs(ctable[k][0][3] / table[k][0][3] - 1) for k in table]
        print(f"\nC++ vs ngspice: THD relative diff median {np.median(errs)*100:.1f}%, max {max(errs)*100:.1f}%; "
              f"output level max diff {max(gains)*100:.3f}%")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5))
    for f in freqs:
        xs = [20 * np.log10(a / np.sqrt(2) / 0.775) for a in levels if (a, f) in table]
        ys = [table[(a, f)][0][0] for a in levels if (a, f) in table]
        line, = ax1.semilogy(xs, ys, "o-", label=f"{f} Hz")
        if ctable:
            ax1.semilogy(xs, [ctable[(a, f)][0][0] for a in levels if (a, f) in table], "x--",
                         color=line.get_color(), ms=8)
    ax1.set_xlabel("source level (dBu)")
    ax1.set_ylabel("THD (%)")
    ax1.set_title("Transformer distortion: rises with level, falls with frequency")
    ax1.grid(True, which="both", alpha=0.3)
    ax1.legend()
    for amp in [0.3, 2, 4]:
        if (amp, 20) not in table:
            continue
        d = table[(amp, 20)][1]
        m = d[:, 0] > d[-1, 0] - 1 / 20
        ax2.plot(d[m, 3], d[m, 2], lw=2.2, alpha=0.5, label=f"{amp} V pk @ 20 Hz (ngspice)")
        if (amp, 20) in ctable:
            dc = ctable[(amp, 20)][1]
            mc = dc[:, 0] > dc[-1, 0] - 1 / 20
            ax2.plot(dc[mc, 3], dc[mc, 2], "k--", lw=0.8)
    ax2.set_xlabel("H (A/m)")
    ax2.set_ylabel("B (T)")
    ax2.set_title("B-H loops (last cycle; dashed = realtime C++)" if ctable else "B-H loops (last cycle)")
    ax2.grid(alpha=0.3)
    ax2.legend()
    fig.tight_layout()
    out = os.path.join(HERE, "core_signature.png")
    fig.savefig(out, dpi=110)
    print(f"\nwrote {out}")


if __name__ == "__main__":
    main()
