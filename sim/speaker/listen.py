#!/usr/bin/env python3
"""Listen to the cab model end to end: the calibrated Legend 1258 (calibrate.py) in a
50 l closed box by default; --generic for the provisional speaker, --rigid for a rigid piston:

  synthetic guitar riff -> Phys Fuzz (the built VST3, via plugin_render)
  -> amp voltage -> driver in a closed box (build/speaker_render, per sample)
  -> cone velocity -> mic IR (build/radiation_check) -> WAV

One file per mic position, from the dust cap to the cone's edge, plus the
fuzz straight (no cab) for reference. All peak-normalised to -1 dBFS; the
level differences between positions are printed instead.

  python3 listen.py [out_dir] [--in guitar_di.wav]
"""
import argparse, json, os, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile
from scipy.signal import fftconvolve

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(HERE, ".."))
from render import synth_riff  # noqa: E402  (48 kHz)

FS = 48000
PLUGIN_RENDER = os.path.join(ROOT, "build", "plugin_render_artefacts", "Release", "plugin_render")
FUZZ = os.path.join(ROOT, "build", "products", "phys-fuzz", "PhysFuzz_artefacts", "Release", "VST3", "Phys Fuzz.vst3")
SPEAKER = os.path.join(ROOT, "build", "speaker_render")
RADIATION = os.path.join(ROOT, "build", "radiation_check")
AMP_VOLTS = 20.0   # full scale at the amp's output: ~25 W peak into 8 ohm


def calibrated_args():
    """the Legend 1258 as calibrated (calibrate.py): driver for speaker_render, driver + cone for radiation_check"""
    sys.path.insert(0, HERE)
    import calibrate as cal
    fit = json.load(open(os.path.join(HERE, "reference", "legend1258_fit.json")))
    d = cal.DRIVER
    spk = []
    for k in ["re", "le", "l2", "r2", "bl", "mms", "cms", "rms", "sd"]:
        spk += ["--set", f"{k}={d[k]}"]
    spk += ["--set", "vb=0.05"]   # a 1x12 closed-back box
    rad = ["--radius", str(cal.RADIUS), "--coil", str(cal.COIL), "--sk", str(0.4 / d["cms"]), "--couple", "1", "--vb", "0.05"]
    for k in ["re", "le", "l2", "r2", "bl", "mms", "cms", "rms", "sd"]:
        rad += ["--" + k, str(d[k])]
    for name, _, _, _, flag in cal.PARAMS:
        rad += [flag, str(fit[name])]
    return spk, rad


def write(path, x):
    wavfile.write(path, FS, (x / np.max(np.abs(x)) * 0.89).astype(np.float32))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default=os.path.join(HERE, "renders"))
    ap.add_argument("--in", dest="inp")
    ap.add_argument("--rigid", action="store_true")
    ap.add_argument("--generic", action="store_true", help="the generic provisional speaker instead of the calibrated Legend 1258")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="cab_")

    if a.inp:
        fs, x = wavfile.read(a.inp)
        x = x.astype(np.float64)
        x = x.mean(axis=1) if x.ndim > 1 else x
        assert fs == FS, "48 kHz input please"
        x /= np.max(np.abs(x))
    else:
        x = synth_riff()
        x /= np.max(np.abs(x))
    dry = os.path.join(work, "dry.wav")
    wavfile.write(dry, FS, (0.5 * x).astype(np.float32))
    fuzzed = os.path.join(work, "fuzz.wav")
    subprocess.run([PLUGIN_RENDER, FUZZ, dry, fuzzed], check=True, capture_output=True)
    _, y = wavfile.read(fuzzed)
    y = y.astype(np.float64)
    y = y[:, 0] if y.ndim > 1 else y
    write(os.path.join(a.out, "00_fuzz_no_cab.wav"), y)

    amp = os.path.join(work, "amp.txt")
    vel = os.path.join(work, "vel.txt")
    np.savetxt(amp, AMP_VOLTS * y / np.max(np.abs(y)), fmt="%.7g")
    spk, rad = ([], []) if a.generic or a.rigid else calibrated_args()
    subprocess.run([SPEAKER, "file", amp, vel, "--fs", str(FS), *spk], check=True, capture_output=True)
    u = np.loadtxt(vel)
    print(f"cone velocity peak {np.max(np.abs(u)):.3f} m/s (excursion is not limited yet: linear model)")

    ref = None
    for i, (off, name) in enumerate([(0.0, "cap"), (0.03, "cap_edge"), (0.06, "cone"), (0.09, "outer_cone"), (0.12, "edge")]):
        r = subprocess.run([RADIATION, "ir", "--fs", str(FS), "--n", "4096", "--offset", str(off),
                            "--distance", "0.025", "--capsule", "0.02", "--pattern", "0.5",
                            "--breakup", "0" if a.rigid else "1", *rad],
                           capture_output=True, text=True, check=True)
        ir = np.array([float(v) for v in r.stdout.split()])
        p = fftconvolve(u, ir)[: len(u)]
        rms = np.sqrt(np.mean(p ** 2))
        ref = ref or rms
        print(f"  {name:10s} {off*100:4.0f} cm off centre: level {20*np.log10(rms/ref):+5.1f} dB re cap")
        write(os.path.join(a.out, f"{i+1:02d}_{name}.wav"), p)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
