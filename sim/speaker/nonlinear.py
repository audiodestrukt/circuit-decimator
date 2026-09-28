#!/usr/bin/env python3
"""Large-signal speaker (Devices.h SpeakerMotor): the engine against ngspice
(speaker.cir with nl=1, the same corrections as behavioural sources, solved fully
implicitly) on sine drive from well inside to well past Xmax.

  1. Bl(x) / Bl0: the overhung coil's overlap with the fringed gap field, and
     where it falls to 82 % (the usual definition of a driver's usable excursion)
  2. 50 and 100 Hz at 2 .. 40 V: peak displacement, THD of the cone velocity,
     compression of the fundamental against the linear model, engine vs ngspice

  python3 nonlinear.py [--plot out.png]
"""
import argparse, os, re, subprocess, tempfile
import numpy as np

os.environ["OMP_NUM_THREADS"] = "1"
HERE = os.path.dirname(os.path.abspath(__file__))
DECK = os.path.join(HERE, "speaker.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "speaker_render")
FS = 96000
GAP, XMAX, FRINGE = 7.9e-3, 0.48e-3, 1e-3


def bl_ratio(x):
    def G(u):
        t = np.abs(u) / FRINGE
        return FRINGE * (t + np.log1p(np.exp(-2 * t)) - np.log(2))
    def ovl(p):
        return 0.5 * (G(p + XMAX + GAP) - G(p - XMAX) - G(p + XMAX) + G(p - XMAX - GAP))
    return ovl(x) / ovl(0.0)


def spice(amp, freq, cycles, nl):
    txt = open(DECK).read()
    txt, n = re.subn(r"(?m)^(\.param nl=)(\S+)", rf"\g<1>{nl}", txt)
    assert n
    txt = re.sub(r"(?m)^Vin src 0 .*$",
                 f"Vin src 0 dc 0 sin(0 {amp} {freq})\nBfade fade 0 V={{min(1, time*{freq}/4)}}", txt)
    # the fade-in, as the engine's: scale the source through a B-source instead
    txt = txt.replace(f"Vin src 0 dc 0 sin(0 {amp} {freq})",
                      f"Vin src0 0 dc 0 sin(0 {amp} {freq})\nBin src 0 V={{v(src0)*v(fade)}}")
    dur = cycles / freq
    ctl = (".control\nset wr_singlescale\nset wr_vecnames\n"
           f"tran {1/FS:.6g} {dur:.6g} 0 {1/FS:.6g}\nwrdata sine.dat v(u) v(xx)\nquit\n.endc")
    txt = re.sub(r"(?s)\.control.*\.endc", ctl, txt)
    work = tempfile.mkdtemp(prefix="spknl_")
    open(os.path.join(work, "d.cir"), "w").write(txt)
    r = subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True, text=True)
    path = os.path.join(work, "sine.dat")
    if not os.path.exists(path):
        raise RuntimeError(r.stdout[-1500:])
    d = np.loadtxt(path, skiprows=1)
    return d[:, 0], d[:, 1], d[:, 2]


def engine(amp, freq, cycles, nl):
    out = tempfile.mktemp(suffix=".dat")
    subprocess.run([TOOL, "sine", str(amp), str(freq), str(cycles), out, "--fs", str(FS), "--set", f"nl={nl}"],
                   check=True, capture_output=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return d[:, 0], d[:, 1], d[:, 3]


def analyse(t, y, freq, cycles_used=4):
    """fundamental amplitude and THD (%) over the last whole cycles, uniformly resampled"""
    T = 1 / freq
    t1 = t[-1] - 0.5 / FS
    grid = t1 - cycles_used * T + np.arange(256 * cycles_used) * (cycles_used * T / (256 * cycles_used))
    yy = np.interp(grid, t, y)
    X = np.abs(np.fft.rfft(yy - yy.mean())) / len(yy)
    h = [X[cycles_used * k] for k in range(1, 10)]
    return 2 * h[0], 100 * np.sqrt(sum(v * v for v in h[1:])) / h[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plot")
    a = ap.parse_args()
    print("1. Bl(x) / Bl0  (gap 7.9 mm, overhang Xmax 0.48 mm, fringing 1 mm)")
    xs = np.array([0, 0.25, 0.48, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0]) * 1e-3
    print("   x mm  " + " ".join(f"{x*1e3:6.2f}" for x in xs))
    print("   Bl    " + " ".join(f"{bl_ratio(x):6.3f}" for x in xs))
    xf = np.linspace(0, 6e-3, 6001)
    x82 = xf[np.argmax(bl_ratio(xf) < 0.82)]
    print(f"   falls to 82 % at {x82*1e3:.2f} mm")

    print("\n2. sine drive, closed 50 l box: engine vs ngspice (implicit)")
    print("   freq   amp V   x peak mm (eng/spice)   THD % velocity (eng/spice)   compression dB (eng/spice)   wave err")
    rows = []
    for freq in [50, 100]:
        cycles = 40
        for amp in [2, 10, 20, 40]:
            ts, us, xs_ = spice(amp, freq, cycles, 1)
            te, ue, xe = engine(amp, freq, cycles, 1)
            tl, ul, _ = engine(amp, freq, cycles, 0)
            fe, the = analyse(te, ue, freq)
            fs_, ths = analyse(ts, us, freq)
            fl, _ = analyse(tl, ul, freq)
            tail = te > te[-1] - 4 / freq
            ui = np.interp(te[tail], ts, us)
            werr = np.sqrt(np.mean((ue[tail] - ui) ** 2)) / np.sqrt(np.mean(ui ** 2))
            xpe = np.max(np.abs(xe[tail]))
            xps = np.max(np.abs(np.interp(te[tail], ts, xs_)))
            ce, cs = 20 * np.log10(fe / fl), 20 * np.log10(fs_ / fl)
            print(f"   {freq:4d}   {amp:5.0f}   {xpe*1e3:6.2f} / {xps*1e3:6.2f}          {the:6.2f} / {ths:6.2f}"
                  f"                {ce:+6.2f} / {cs:+6.2f}             {werr:.1e}")
            rows.append((freq, amp, te, ue, ts, us))

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        fig, ax = plt.subplots(1, 2, figsize=(13, 4.5))
        xf2 = np.linspace(-5e-3, 5e-3, 1001)
        ax[0].plot(xf2 * 1e3, bl_ratio(xf2))
        ax[0].axvspan(-XMAX * 1e3, XMAX * 1e3, alpha=0.15, label="Xmax (datasheet)")
        ax[0].axhline(0.82, ls="--", lw=0.8, color="k")
        ax[0].set_xlabel("cone displacement (mm)"); ax[0].set_ylabel("Bl / Bl0"); ax[0].set_title("force factor vs excursion")
        ax[0].legend(); ax[0].grid(alpha=0.3)
        for freq, amp, te, ue, ts, us in rows:
            if freq == 50 and amp in (10, 40):
                tail = te > te[-1] - 2 / freq
                ax[1].plot((te[tail] - te[tail][0]) * 1e3, ue[tail], lw=1.2, label=f"engine {amp} V")
                m = ts > te[-1] - 2 / freq
                ax[1].plot((ts[m] - te[tail][0]) * 1e3, us[m], "--", lw=1, label=f"ngspice {amp} V")
        ax[1].set_xlabel("ms"); ax[1].set_ylabel("cone velocity (m/s)"); ax[1].set_title("50 Hz, 2 cycles")
        ax[1].legend(fontsize=8); ax[1].grid(alpha=0.3)
        fig.tight_layout()
        fig.savefig(a.plot, dpi=110)


if __name__ == "__main__":
    main()
