#!/usr/bin/env python3
"""Search the destroy-knob space for distinct, usable sounds (MAP-Elites).

  python3 explore.py                        # default run: brightness x sputter
  python3 explore.py --axes sputter asymmetry --budget 6000 --name gated
  python3 explore.py --input di.wav         # search with your own playing
  python3 explore.py --probe                # just print features of the presets

How it works: a Sobol scatter over the searchable knobs (normalised 0..1, the
same curves the plugin's knobs use), then MAP-Elites -- the sounds are placed
on a grid by two character descriptors, each cell keeps its best-scoring sound,
and elites are mutated/crossed to climb within cells and discover new ones. A
final polish pass does small local steps around every elite.

"Best" is a usability score, not taste: the sound is alive when played, quiet
when you stop, follows your picking, isn't pure noise, and the solver is happy
with it. Taste is the grid: listen to the cells (index.html, or Circuit Bench
"Load search...") and keep what you like.

Output: search/runs/<name>/  elites.json  map.png  index.html  audio/*.wav
"""
import argparse, ctypes, json, os, sys, time
from concurrent.futures import ProcessPoolExecutor
import numpy as np
from scipy.io import wavfile
from scipy.signal import butter, resample_poly, sosfilt, welch
from scipy.stats import qmc

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "sim"))
import render  # noqa: E402  (synthetic riff + DI loading)

FS = 48000
OS = 4
TAIL = 0.5          # seconds of silence appended to the input: self-oscillation check
TARGET_RMS_DB = -18  # elites get an Output knob that plays them at this level


# ------------------------------------------------------------------ solver lib
class Solver:
    def __init__(self):
        path = os.path.join(ROOT, "build", "libfuzzface.so")
        if not os.path.exists(path):
            raise SystemExit(f"{path} missing: cmake --build build --target fuzzface")
        l = self.lib = ctypes.CDLL(path)
        l.ff_knob_id.restype = ctypes.c_char_p
        l.ff_knob_unit.restype = ctypes.c_char_p
        l.ff_preset_name.restype = ctypes.c_char_p
        for f in ("ff_knob_min", "ff_knob_max", "ff_knob_default",
                  "ff_knob_from_normalised", "ff_knob_to_normalised"):
            getattr(l, f).restype = ctypes.c_double
        l.ff_knob_from_normalised.argtypes = [ctypes.c_int, ctypes.c_double]
        l.ff_knob_to_normalised.argtypes = [ctypes.c_int, ctypes.c_double]
        dp = np.ctypeslib.ndpointer(np.float64, flags="C")
        fp = np.ctypeslib.ndpointer(np.float32, flags="C")
        l.ff_render.argtypes = [dp, fp, fp, ctypes.c_int, ctypes.c_double, ctypes.c_int, dp]
        l.ff_preset_knobs.argtypes = [ctypes.c_int, dp]

        n = l.ff_num_knobs()
        self.ids = [l.ff_knob_id(i).decode() for i in range(n)]
        self.units = [l.ff_knob_unit(i).decode() for i in range(n)]
        self.defaults = np.array([l.ff_knob_default(i) for i in range(n)])
        self.search = [i for i in range(n) if l.ff_knob_searchable(i)]

    def knobs(self, genome):
        """normalised genome (searchable knobs only) -> all knob values"""
        v = self.defaults.copy()
        for g, i in zip(genome, self.search):
            v[i] = self.lib.ff_knob_from_normalised(i, float(g))
        return v

    def genome(self, knobs):
        return np.array([self.lib.ff_knob_to_normalised(i, float(knobs[i])) for i in self.search])

    def presets(self):
        out = []
        for i in range(self.lib.ff_num_presets()):
            v = np.zeros(len(self.ids))
            self.lib.ff_preset_knobs(i, v)
            out.append((self.lib.ff_preset_name(i).decode(), v))
        return out

    def render(self, knobs, x_up, iters=8):
        out = np.empty_like(x_up)
        stats = np.zeros(3)
        self.lib.ff_render(np.ascontiguousarray(knobs, np.float64), x_up, out, len(x_up),
                           FS * OS, iters, stats)
        return out, stats


# ------------------------------------------------------------------ features
# Each descriptor: how it's computed lives in features(); here are labels and
# the words used to name elites by where they sit along that axis.
DESCRIPTORS = {
    "brightness": ("brightness (spectral centroid, log2 Hz)", ["dark", "warm", "mid", "bright", "fizzy"]),
    "sputter":    ("sputter (envelope choppiness, dB/5ms)", ["smooth", "grainy", "gated", "sputtery", "broken"]),
    "asymmetry":  ("asymmetry (waveform offset)", ["even", "leaning", "lopsided", "one-sided", "rectified"]),
    "dynamics":   ("dynamics (output follows picking, corr)", ["flat", "squashed", "springy", "touchy", "open"]),
    "noisiness":  ("noisiness (spectral flatness)", ["tonal", "hairy", "buzzy", "noisy", "static"]),
}


def envelope_db(s, frame, hop):
    """Moving-average power in dB, one value per hop (running-sum, O(n))."""
    c = np.concatenate([[0.0], np.cumsum(s.astype(np.float64) ** 2)])
    p = (c[frame:] - c[:-frame]) / frame
    p = np.concatenate([np.full(frame // 2, p[0]), p])  # centre the window
    return 10 * np.log10(np.maximum(p[::hop], 0) + 1e-12)


def features(y, x, n_riff, stats):
    """y, x: output / input at FS. n_riff: samples before the silent tail."""
    hop, frame = int(0.005 * FS), int(0.030 * FS)
    ex, ey = envelope_db(x, frame, hop), envelope_db(y, frame, hop)
    t_riff = n_riff // hop
    active = np.zeros(len(ex), bool)
    active[:t_riff] = ex[:t_riff] > ex.max() - 30
    idle = np.arange(len(ex)) > t_riff + int(0.15 * FS / hop)

    f = {}
    f["level"] = float(10 * np.log10(np.mean(10 ** (ey[active] / 10)) + 1e-12))
    f["alive"] = float(np.mean(ey[active] > -45))
    f["idle_db"] = float(10 * np.log10(np.mean(10 ** (ey[idle] / 10)) + 1e-12))

    fr, P = welch(y[:n_riff], FS, nperseg=4096)
    band = (fr > 40) & (fr < 12000)
    Pb = P[band] + 1e-20
    f["brightness"] = float(np.log2(np.sum(fr[band] * Pb) / np.sum(Pb)))
    f["noisiness"] = float(np.exp(np.mean(np.log(Pb))) / np.mean(Pb))

    both = active[1:] & active[:-1]
    dy = np.abs(np.diff(ey))[both]
    dx = np.abs(np.diff(ex))[both]
    f["sputter"] = float(np.mean(dy) - np.mean(dx)) if both.any() else 0.0
    f["dynamics"] = float(np.corrcoef(ex[active], ey[active])[0, 1]) if np.std(ey[active]) > 1e-6 else 0.0

    yr = y[:n_riff]
    hi, lo = np.percentile(yr, 99.5), np.percentile(yr, 0.5)
    f["asymmetry"] = float(abs(hi + lo) / (hi - lo + 1e-9))

    f["newton_avg"], f["newton_max"], f["failures"] = (float(s) for s in stats)
    for k, v in f.items():
        if not np.isfinite(v):
            f[k] = 0.0
    return f


def clip01(v):
    return float(min(1.0, max(0.0, v)))


def fitness(f):
    """Usability in 0..1 (not taste). Each factor is a soft gate."""
    alive = clip01(f["alive"] / 0.4)                  # sounds when played (gating allowed)
    level = clip01((f["level"] + 55) / 20)             # not buried in the noise floor
    quiet = 1 - clip01((f["idle_db"] + 60) / 20)       # silent when you stop playing
    tonal = 1 - 0.8 * clip01((f["noisiness"] - 0.4) / 0.4)    # not pure hiss (sputter is spiky, allow it)
    touch = 0.8 + 0.2 * clip01(f["dynamics"])          # follows picking (mildly: fuzz compresses)
    solver = 1.0 if f["failures"] == 0 and f["newton_avg"] < 3.5 else 0.5
    return alive * level * quiet * tonal * touch * solver


# ------------------------------------------------------------------ workers
_solver = _x = _xup = _n_riff = _hp = None


def _init(x, n_riff):
    global _solver, _x, _xup, _n_riff, _hp
    _solver = Solver()
    _x = x
    _xup = np.ascontiguousarray(resample_poly(x, OS, 1), np.float32)
    _n_riff = n_riff
    _hp = butter(2, 15, "highpass", fs=FS, output="sos")


def _output(knobs):
    yu, stats = _solver.render(knobs, _xup)
    y = sosfilt(_hp, resample_poly(yu, 1, OS))
    return y, stats


def evaluate(genome):
    knobs = _solver.knobs(genome)
    y, stats = _output(knobs)
    f = features(y, _x, _n_riff, stats)
    return genome, f, fitness(f)


def evaluate_knobs(knobs):
    y, stats = _output(np.asarray(knobs))
    f = features(y, _x, _n_riff, stats)
    return f, fitness(f)


def render_audio(knobs):
    return _output(np.asarray(knobs))[0].astype(np.float32)


# ------------------------------------------------------------------ MAP-Elites
class Archive:
    def __init__(self, axes, bounds, grid):
        self.axes, self.bounds, self.grid = axes, bounds, grid
        self.cells = {}

    def cell(self, f):
        idx = []
        for a, (lo, hi) in zip(self.axes, self.bounds):
            idx.append(int(np.clip((f[a] - lo) / (hi - lo) * self.grid, 0, self.grid - 1)))
        return tuple(idx)

    def add(self, genome, f, fit):
        """Insert if the cell is empty or this beats its elite. Returns True if kept."""
        if fit <= 0:
            return False
        c = self.cell(f)
        cur = self.cells.get(c)
        if cur is None or fit > cur["fitness"]:
            self.cells[c] = {"genome": np.asarray(genome), "features": f, "fitness": fit}
            return True
        return False

    def qd_score(self):
        return sum(e["fitness"] for e in self.cells.values())


def variation(archive, n, rng, iso=0.06, line=0.25):
    """Iso+line operator (Vassiliades & Mouret 2018): gaussian step plus a step
    along the line to another elite, which exploits correlations between knobs."""
    elites = list(archive.cells.values())
    d = len(elites[0]["genome"])
    kids = []
    for _ in range(n):
        a, b = rng.choice(len(elites), 2)
        xa, xb = elites[a]["genome"], elites[b]["genome"]
        child = xa + iso * rng.standard_normal(d) + line * rng.standard_normal() * (xb - xa)
        kids.append(np.clip(child, 0, 1))
    return kids


def polish(archive, per_elite, rng, sigma=0.02):
    kids = []
    for e in archive.cells.values():
        for _ in range(per_elite):
            kids.append(np.clip(e["genome"] + sigma * rng.standard_normal(len(e["genome"])), 0, 1))
    return kids


# ------------------------------------------------------------------ outputs
def name_for(archive, cell):
    words = []
    for a, i in zip(archive.axes, cell):
        w = DESCRIPTORS[a][1]
        words.append(w[min(len(w) - 1, i * len(w) // archive.grid)])
    return " ".join(words) + f" {cell[0]}.{cell[1]}"


def write_outputs(run_dir, archive, history, solver, pool, preset_points, args):
    os.makedirs(os.path.join(run_dir, "audio"), exist_ok=True)
    cells = sorted(archive.cells)
    elites = []
    knob_sets = []
    for c in cells:
        e = archive.cells[c]
        knobs = solver.knobs(e["genome"])
        out_db = float(np.clip(TARGET_RMS_DB - e["features"]["level"], -36, 30))
        knobs[solver.ids.index("output")] = round(out_db, 1)
        knob_sets.append(knobs)
        elites.append({
            "name": name_for(archive, c),
            "cell": list(c),
            "fitness": round(e["fitness"], 4),
            "knobs": {k: round(float(v), 4) for k, v in zip(solver.ids, knobs)},
            "features": {k: round(v, 4) for k, v in e["features"].items()},
            "audio": f"audio/{c[0]}_{c[1]}.wav",
        })
    for el, y in zip(elites, pool.map(render_audio, knob_sets)):
        wavfile.write(os.path.join(run_dir, el["audio"]), FS, np.clip(y, -1, 1))

    doc = {
        "axes": archive.axes,
        "axis_labels": [DESCRIPTORS[a][0] for a in archive.axes],
        "bounds": [list(b) for b in archive.bounds],
        "grid": archive.grid,
        "evaluations": len(history),
        "seed": args.seed,
        "input": args.input or "synthetic riff",
        "presets": [{"name": n, "features": {a: round(f[a], 4) for a in archive.axes},
                     "fitness": round(fit, 4)} for n, f, fit in preset_points],
        "elites": elites,
    }
    with open(os.path.join(run_dir, "elites.json"), "w") as fh:
        json.dump(doc, fh, indent=1)
    plot_map(os.path.join(run_dir, "map.png"), archive, history, preset_points)
    write_html(os.path.join(run_dir, "index.html"), doc)


def plot_map(path, archive, history, preset_points):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    (ax0, ax1), g = archive.axes, archive.grid
    (lo0, hi0), (lo1, hi1) = archive.bounds
    fig, (a, b) = plt.subplots(1, 2, figsize=(14, 6))

    img = np.full((g, g), np.nan)
    for (i, j), e in archive.cells.items():
        img[j, i] = e["fitness"]
    im = a.pcolormesh(np.linspace(lo0, hi0, g + 1), np.linspace(lo1, hi1, g + 1),
                      np.ma.masked_invalid(img), cmap="viridis", vmin=0, vmax=1,
                      edgecolors="0.25", linewidth=0.5)
    a.set_xlim(lo0, hi0)
    a.set_ylim(lo1, hi1)
    fig.colorbar(im, ax=a, label="usability")
    a.set_title(f"elites: {len(archive.cells)}/{g * g} cells, QD score {archive.qd_score():.1f}")

    hx = np.array([h[1][ax0] for h in history])
    hy = np.array([h[1][ax1] for h in history])
    hf = np.array([h[2] for h in history])
    b.scatter(hx, hy, c=hf, s=4, cmap="viridis", vmin=0, vmax=1, alpha=0.6)
    b.set_xlim(lo0, hi0)
    b.set_ylim(lo1, hi1)
    b.set_title(f"all {len(history)} evaluations")

    for ax in (a, b):
        ax.set_xlabel(DESCRIPTORS[ax0][0])
        ax.set_ylabel(DESCRIPTORS[ax1][0])
        for name, f, _ in preset_points:
            ax.plot(f[ax0], f[ax1], "o", mfc="none", mec="tab:red", ms=7)
            ax.annotate(name, (f[ax0], f[ax1]), fontsize=6, color="tab:red",
                        xytext=(3, 3), textcoords="offset points")
    fig.tight_layout()
    fig.savefig(path, dpi=110)


def write_html(path, doc):
    html = HTML_TEMPLATE.replace("__DATA__", json.dumps(doc))
    with open(path, "w") as fh:
        fh.write(html)


HTML_TEMPLATE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Circuit Decimator Search</title>
<style>
:root { --bg:#161719; --panel:#1f2124; --fg:#e6e6e6; --dim:#8a8f98; --line:#34373c; --accent:#7fd18b; }
body { margin:0; background:var(--bg); color:var(--fg); font:14px/1.4 system-ui, sans-serif; }
main { max-width:1100px; margin:0 auto; padding:16px; display:grid; grid-template-columns:minmax(0,1fr) 320px; gap:16px; }
@media (max-width:800px) { main { grid-template-columns:1fr; } }
h1 { font-size:18px; margin:0 0 4px; } .dim { color:var(--dim); font-size:12px; }
.map { display:grid; gap:3px; margin-top:8px; }
.cell { aspect-ratio:1; border-radius:3px; background:var(--panel); border:1px solid var(--line); cursor:pointer; padding:0; }
.cell.empty { cursor:default; opacity:.35; }
.cell.playing { outline:2px solid #fff; }
.axis { display:flex; justify-content:space-between; color:var(--dim); font-size:12px; margin-top:4px; }
.side { background:var(--panel); border:1px solid var(--line); border-radius:6px; padding:12px; align-self:start; }
table { width:100%; border-collapse:collapse; font-size:12px; } td { padding:2px 0; } td:last-child { text-align:right; font-variant-numeric:tabular-nums; }
audio { width:100%; margin:8px 0; }
.ylab { writing-mode:vertical-rl; transform:rotate(180deg); color:var(--dim); font-size:12px; text-align:center; }
.wrap { display:grid; grid-template-columns:20px 1fr; gap:6px; }
</style></head><body><main>
<section>
  <h1>Circuit Decimator: search map</h1>
  <div class="dim" id="meta"></div>
  <div class="wrap"><div class="ylab" id="ylab"></div><div><div class="map" id="map"></div>
  <div class="axis"><span id="xlo"></span><span id="xlab"></span><span id="xhi"></span></div></div></div>
</section>
<aside class="side">
  <div id="name" style="font-weight:600">click a cell to listen</div>
  <div class="dim" id="fit"></div>
  <audio id="player" controls loop></audio>
  <table id="knobs"></table>
  <div class="dim" style="margin-top:8px">Load elites.json in Circuit Bench ("Load search...") to play these live.</div>
</aside>
</main>
<script>
const D = __DATA__;
const g = D.grid, map = document.getElementById('map');
map.style.gridTemplateColumns = `repeat(${g}, 1fr)`;
document.getElementById('meta').textContent =
  `${D.elites.length} sounds from ${D.evaluations} evaluations · input: ${D.input} · colour = usability`;
document.getElementById('xlab').textContent = D.axis_labels[0];
document.getElementById('ylab').textContent = D.axis_labels[1];
document.getElementById('xlo').textContent = D.bounds[0][0].toFixed(2);
document.getElementById('xhi').textContent = D.bounds[0][1].toFixed(2);
const byCell = {}; D.elites.forEach(e => byCell[e.cell.join(',')] = e);
function colour(f) { const h = 260 - 180 * f; return `hsl(${h} 60% ${25 + 30 * f}%)`; }
let current = null;
for (let j = g - 1; j >= 0; j--) for (let i = 0; i < g; i++) {
  const e = byCell[i + ',' + j], b = document.createElement('button');
  b.className = 'cell' + (e ? '' : ' empty');
  if (e) { b.style.background = colour(e.fitness); b.title = e.name; b.onclick = () => pick(e, b); }
  map.appendChild(b);
}
function pick(e, b) {
  if (current) current.classList.remove('playing'); current = b; b.classList.add('playing');
  document.getElementById('name').textContent = e.name;
  document.getElementById('fit').textContent = `usability ${e.fitness.toFixed(2)} · ` +
    D.axes.map(a => `${a} ${e.features[a].toFixed(2)}`).join(' · ');
  const p = document.getElementById('player'); p.src = e.audio; p.play();
  document.getElementById('knobs').innerHTML = Object.entries(e.knobs)
    .map(([k, v]) => `<tr><td>${k}</td><td>${v}</td></tr>`).join('');
}
</script></body></html>
"""


# ------------------------------------------------------------------ main
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--axes", nargs=2, default=["brightness", "sputter"], choices=list(DESCRIPTORS))
    ap.add_argument("--grid", type=int, default=10)
    ap.add_argument("--init", type=int, default=512, help="Sobol scatter size (power of 2)")
    ap.add_argument("--budget", type=int, default=4000, help="total evaluations before polish")
    ap.add_argument("--batch", type=int, default=224)
    ap.add_argument("--polish", type=int, default=6, help="local steps per elite at the end")
    ap.add_argument("--input", help="DI wav (default: synthetic riff)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--name", default=time.strftime("%Y%m%d-%H%M%S"))
    ap.add_argument("--workers", type=int, default=os.cpu_count())
    ap.add_argument("--probe", action="store_true", help="print preset features and exit")
    args = ap.parse_args()

    x = render.load_input(args.input) / 0.15  # load_input scales to 150 mV; back to full scale
    n_riff = len(x)
    x = np.concatenate([x, np.zeros(int(TAIL * FS))]).astype(np.float32)
    solver = Solver()
    rng = np.random.default_rng(args.seed)
    t0 = time.time()

    with ProcessPoolExecutor(args.workers, initializer=_init, initargs=(x, n_riff)) as pool:
        presets = solver.presets()
        preset_points = [(n, f, fit) for (n, _), (f, fit) in
                         zip(presets, pool.map(evaluate_knobs, [k for _, k in presets]))]
        if args.probe:
            keys = ["level", "alive", "idle_db", "brightness", "sputter", "asymmetry", "dynamics",
                    "noisiness", "newton_avg"]
            print(f"{'preset':20s} " + " ".join(f"{k:>10s}" for k in keys) + "   usable")
            for n, f, fit in preset_points:
                print(f"{n:20s} " + " ".join(f"{f[k]:10.3f}" for k in keys) + f"   {fit:.2f}")
            return

        d = len(solver.search)
        scatter = qmc.Sobol(d, scramble=True, seed=args.seed).random(args.init)
        history = list(pool.map(evaluate, scatter, chunksize=4))

        # grid bounds: 2..98th pct of the usable scatter, widened to take in the
        # hand-made presets -- the scatter rarely lands near healthy settings,
        # and the classic-fuzz corner shouldn't be squashed into an edge row
        usable = [h for h in history if h[2] > 0.05] or history
        bounds = []
        for a in args.axes:
            v = np.array([h[1][a] for h in usable])
            pv = np.array([f[a] for _, f, _ in preset_points])
            lo = min(np.percentile(v, 2), pv.min())
            hi = max(np.percentile(v, 98), pv.max())
            pad = 0.02 * (hi - lo)
            bounds.append((float(lo - pad), float(hi + pad if hi > lo else lo + 1e-3)))
        archive = Archive(args.axes, bounds, args.grid)
        for h in history:
            archive.add(*h)
        print(f"scatter: {len(history)} evals, {len(archive.cells)} cells, "
              f"QD {archive.qd_score():.1f}  ({time.time() - t0:.0f}s)")

        while len(history) < args.budget:
            kids = variation(archive, min(args.batch, args.budget - len(history)), rng)
            batch = list(pool.map(evaluate, kids, chunksize=4))
            kept = sum(archive.add(*h) for h in batch)
            history += batch
            print(f"  {len(history):5d} evals  {len(archive.cells):3d} cells  QD {archive.qd_score():6.1f}  "
                  f"improved {kept:3d}  ({time.time() - t0:.0f}s)")

        if args.polish:
            batch = list(pool.map(evaluate, polish(archive, args.polish, rng), chunksize=4))
            kept = sum(archive.add(*h) for h in batch)
            history += batch
            print(f"polish: {len(batch)} evals, improved {kept}, QD {archive.qd_score():.1f}")

        run_dir = os.path.join(HERE, "runs", args.name)
        write_outputs(run_dir, archive, history, solver, pool, preset_points, args)
    print(f"\n{len(archive.cells)} sounds in {run_dir}/  ({time.time() - t0:.0f}s total)")
    print(f"  open {run_dir}/index.html   or load elites.json in Circuit Bench")


if __name__ == "__main__":
    main()
