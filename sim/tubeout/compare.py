#!/usr/bin/env python3
"""Single-ended output stage: ngspice (se_out.cir) vs the engine (build/circuit_render seout).

  python3 compare.py                 # bias, gain + THD (even/odd) vs level and frequency
  python3 compare.py --power-on      # how the power-on ramp changes the core's resting state
  python3 compare.py --listen        # riff / bass through the stage: core vs linear iron, gap sweep
"""
import argparse, os, re, subprocess, sys, tempfile
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, resample_poly, sosfilt

# ngspice's OpenMP threading makes runs non-deterministic; see ../tubepre/README.md
os.environ["OMP_NUM_THREADS"] = "1"

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transformer"))
sys.path.insert(0, os.path.join(HERE, ".."))
from core_sweep import thd  # noqa: E402

DECK = os.path.join(HERE, "se_out.cir")
TOOL = os.path.join(HERE, "..", "..", "build", "circuit_render")


def spice(amp, freq, tramp=3.0, lg=None):
    tsig = tramp + 0.3
    over = {"amp": amp, "freq": freq, "tramp": tramp, "tsig": tsig}
    if lg is not None:
        over["lg"] = lg
    txt = open(DECK).read()
    for k, v in over.items():
        txt, n = re.subn(rf"(?m)(^\.param\b.*?\b{k}=)(\S+)", rf"\g<1>{v:.6g}", txt)
        assert n, k
    dt = min(1 / (freq * 400), 2e-5)
    txt = re.sub(r"(?m)^tran .*", f"tran {dt:.6g} {tsig + 12 / freq:.6g} 0 {dt:.6g} uic", txt)
    work = tempfile.mkdtemp(prefix="se_")
    open(os.path.join(work, "d.cir"), "w").write(txt)
    subprocess.run(["ngspice", "-b", "d.cir"], cwd=work, capture_output=True)
    return np.loadtxt(os.path.join(work, "se.dat"), skiprows=1), tsig


def engine(args):
    amp, freq, lg = args
    out = tempfile.mktemp(suffix=".dat")
    cmd = [TOOL, "seout", "sine", str(amp), str(freq), "12", out] + (["--set", f"lg={lg}"] if lg else [])
    r = subprocess.run(cmd, capture_output=True, text=True, check=True)
    d = np.loadtxt(out, skiprows=1)
    os.remove(out)
    return d, r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--power-on", action="store_true")
    ap.add_argument("--listen", action="store_true")
    a = ap.parse_args()

    if a.power_on:
        for tramp in [0.02, 0.06, 0.2, 1.0]:
            d, tsig = spice(0.001, 50, tramp)
            m = (d[:, 0] > tsig - 0.1) & (d[:, 0] < tsig)
            t = d[:, 0]
            i = (250 * np.minimum(t / tramp, 1) - d[:, 4]) / 800
            print(f"B+ ramp {tramp * 1e3:6.0f} ms: plate current peak {abs(i[t < tsig]).max() * 1e3:5.2f} mA, "
                  f"settled {abs(i[m]).mean() * 1e3:.2f} mA -> core B {d[m, 2].mean():+.4f} T, H {d[m, 3].mean():+.2f} A/m")
        _, log = engine((0.001, 50, None))
        print("engine (initDC, monotonic power-on):", log.splitlines()[0])
        return

    if a.listen:
        listen()
        return

    levels = [0.3, 1, 3, 6]            # V peak at the grid source
    freqs = [30, 100, 1000]
    jobs = [(amp, f, None) for f in freqs for amp in levels]
    sp = [spice(amp, f) for amp, f, _ in jobs]          # ngspice one at a time
    with ProcessPoolExecutor() as ex:
        en = list(ex.map(engine, jobs))
    print(en[0][1].splitlines()[0].replace("bias:", "engine bias:"))
    d0, tsig0 = sp[0]
    m = (d0[:, 0] > tsig0 - 0.1) & (d0[:, 0] < tsig0)
    print(f"ngspice bias: plate {d0[m, 4].mean():.2f} V, B_dc {d0[m, 2].mean():+.4f} T, H_dc {d0[m, 3].mean():+.2f} A/m\n")
    print(f"{'V pk':>5s} {'freq':>5s} | {'gain dB (spice/engine)':>23s} | {'THD % (spice/engine)':>21s} | even/odd % engine")
    for (amp, f, _), (ds, _), (de, _) in zip(jobs, sp, en):
        ts, os_, es_, fs_ = thd(ds[:, 0], ds[:, 1], f)
        te, oe, ee, fe = thd(de[:, 0], de[:, 1], f)
        print(f"{amp:5.1f} {f:5d} | {20 * np.log10(fs_ / amp):10.2f} / {20 * np.log10(fe / amp):8.2f}  | "
              f"{ts:9.3f} / {te:8.3f}  | {ee:.3f} / {oe:.3f}")


def render_file(args):
    name, x_up, extra = args
    inp, out = tempfile.mktemp(suffix=".txt"), tempfile.mktemp(suffix=".dat")
    np.savetxt(inp, np.column_stack([np.arange(len(x_up)) / 192000, x_up]), fmt="%.9g")
    subprocess.run([TOOL, "seout", "file", inp, out] + extra, capture_output=True, check=True)
    y = np.loadtxt(out, skiprows=1)[:, 1]
    os.remove(inp)
    os.remove(out)
    return name, resample_poly(y, 1, 4)


def listen():
    """riff + bass at 2 V peak into the stage: real core, ideal core, and gap variants"""
    import render
    from listen import bass_line
    out_dir = os.path.join(HERE, "renders")
    os.makedirs(out_dir, exist_ok=True)
    hp = butter(2, 20, "highpass", fs=48000, output="sos")
    variants = {"core": [], "linear": ["--linear"], "gap0.05mm": ["--set", "lg=5e-5"], "gap0.2mm": ["--set", "lg=2e-4"]}
    jobs = []
    for sname, x in {"riff": render.synth_riff(), "bass": bass_line()}.items():
        x = sosfilt(hp, x)
        x_up = resample_poly(2.0 * x / np.max(np.abs(x)), 4, 1)
        for vname, extra in variants.items():
            jobs.append((f"{sname}_{vname}", x_up, extra))
    with ProcessPoolExecutor() as ex:
        got = dict(ex.map(render_file, jobs))
    for sname in ("riff", "bass"):
        ys = {v: sosfilt(hp, got[f"{sname}_{v}"]) for v in variants}
        g = 0.89 / max(np.abs(y).max() for y in ys.values())   # one gain per source: levels comparable
        ref = ys["linear"]
        for v, y in ys.items():
            wavfile.write(os.path.join(out_dir, f"{sname}_{v}.wav"), 48000, (y * g).astype(np.float32))
            if v != "linear":
                rel = 20 * np.log10(np.sqrt(np.mean((y - ref) ** 2)) / np.sqrt(np.mean(ref**2)))
                print(f"{sname}_{v:10s} differs from ideal iron by {rel:6.1f} dB")
    print(f"wrote {out_dir}/")


if __name__ == "__main__":
    main()
