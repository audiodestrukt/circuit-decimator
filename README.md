# Circuit Decimator

A VST3 fuzz whose "knobs" are component failures: a realtime circuit simulation
of a Fuzz Face (or a Shin-Ei FY-2) where you can starve the battery, leak the
junctions, cook the transistors and automate all of it.

Product names in this repo (Fuzz Face, Shin-Ei, FY-2, LA-2A, ...) identify the
circuits modeled; they are trademarks of their owners and there is no
affiliation. Nothing a player sees uses them: the plugins name circuits by
origin ("London '66", "Tokyo '68").

A monorepo: one shared circuit core, one folder per product.

```
core/circuit/        FuzzFaceDK.h (realtime solver: nodal DK method, double or float, no JUCE/heap)
                     FuzzFace.h (reference MNA solver + shared transistor model; DC operating point)
                     Knobs.h (full control surface: ranges/curves/presets -> circuit values)
                     circuits/ShinEi.h (Shin-Ei FY-2 as a netlist on the general engine, same knobs)
core/engine/         FuzzEngine: the shared audio path (4x oversampling, smoothing, telemetry)
core/ui/             CircuitView: live schematic (node voltages, signal glow, damage in red)
products/workbench/  Circuit Decimator plugin (every component knob) + Circuit Bench app
products/phys-fuzz/  Phys Fuzz plugin: Battery / Age / Temperature macros over the core;
                     Fuzz Face or Shin-Ei FY-2 (reference/ holds the FY-2 schematic)
products/iron/       Iron plugin: tube line stage into a gapped single-ended output transformer
sim/                 ngspice deck (the circuit spec), reference renders, solver-vs-ngspice check
search/              MAP-Elites sound search over the knob space
tools/               ff_render, libfuzzface (C API for Python), plugin_render (headless VST3 host),
                     embedded/ (float core behind a C API; Cortex-M7 compile + codegen check)
```

A product is a thin JUCE target: its own knob table (a `cd::Knob` array), a
mapping onto the core's knob surface (e.g. `products/phys-fuzz/Macros.h`),
presets, an editor, and its own plugin code in its CMakeLists.

## Build

JUCE 9.0.2 lives in `./JUCE`
(`git clone --depth 1 --branch 9.0.2 https://github.com/juce-framework/JUCE.git JUCE`).

```
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build          # installs "Circuit Decimator.vst3" and "Phys Fuzz.vst3" into ~/.vst3
```

## Test bench

```
"build/products/workbench/CircuitBench_artefacts/Release/Circuit Bench" [file.wav]
```

Loops a WAV through the processor (default: `sim/renders/input.wav`, loaded
paused; drop any DI file on the window to play it), or switch to live input.
Bypass A/Bs the dry signal, the preset menu holds the destroy presets, and
"Power cycle" re-solves the circuit's DC state after you've biased it to death.
The stats line shows CPU, Newton iterations per sample and solver failures.

The **Circuit** menu switches between the fuzz workbench and the netlist
circuits (tube mic pre, single-ended output stage = Iron's circuit, mic
transformer, LA-2A, Tokyo '68 = the FY-2 fuzz). For those, every component is a knob in a scrolling panel
(transformer ratio, leakage, DCR, capacitances, load; core turns, area, path,
gap and Jiles-Atherton steel; tube stage resistors, caps, B+ and the Koren curve),
applied live without losing the circuit's state. A node meter shows each probe
node's DC voltage and signal swing, and circuits with a core show the live B-H
loop. `--circuit N` opens one directly (1 tube pre, 2 SE output, 3 mic
transformer, 4 LA-2A, 5 FY-2 fuzz). Circuits live in `core/circuit/circuits/Catalog.h`.

For the fuzz workbench, the schematic is live: wire colour is each node's voltage (blue 0 V to amber
9 V), a green glow shows where the signal swings, parts turn red as their knob
leaves healthy, leakage appears as dashed resistors, and Q2's state (biased /
saturated / cut off) is called out. The VST3's editor shows the same view.
`--play --mute --program N` runs it silently, for screenshots and demos.

## Search for sounds

```
cmake --build build --target fuzzface        # the solver as a library for Python
cd search && python3 explore.py --name mine  # ~4 min on 28 cores
```

Scatters random settings over the searchable knobs, then runs MAP-Elites: each
sound is placed on a grid by two character descriptors (default brightness x
sputter; also asymmetry, dynamics, noisiness via `--axes`), each cell keeps its
most usable sound, and elites are mutated to climb within cells and find new
ones. "Usable" means alive when played, silent at rest, not pure noise, and
easy on the solver. It's a gate, not taste; the grid supplies the variety.

Results land in `search/runs/<name>/`: `index.html` (click cells to listen),
`map.png` (the grid, with the hand-made presets as landmarks), `audio/`, and
`elites.json`, which loads into Circuit Bench to play live:

```
"build/products/workbench/CircuitBench_artefacts/Release/Circuit Bench" search/runs/mine/elites.json
```

## Check it

```
cd sim && python3 render.py && python3 compare.py   # ngspice renders, then solver vs ngspice
build/plugin_render_artefacts/Release/plugin_render ~/.vst3/"Circuit Decimator.vst3" \
    sim/renders/input.wav out.wav --ramp battery:9:3.5    # or --program 4
```

Parameter names for `plugin_render` are the display names without spaces
(`junctionleak=0.5`, `temperature=80`).

## Phys Fuzz

![Phys Fuzz](products/phys-fuzz/screenshots/barn-find.png)

The editor is the pedal with its lid off: copper traces on phenolic board that
brighten where the signal is, turn verdigris with Age, and scorch where parts
are failing. Condition knobs sit under the board; the playing knobs and level
run down the right. Fonts: Michroma and Barlow (SIL OFL, in
`products/phys-fuzz/assets/fonts`). Regenerate screenshots with
`physfuzz_snapshot out.png sim/renders/input.wav --program N` (under xvfb-run on
a headless box; `circuit=1` for Tokyo '68).

Six knobs: Fuzz, Volume, **Battery** (charge: voltage falls and sag climbs,
0% is tuned to sputter, not die), **Age** (junction leak, dried caps, fading
gain, cold bias drift), Temperature, Output. The mapping lives in
`products/phys-fuzz/Macros.h` and is a first pass for tuning by ear; it's also
exposed through libfuzzface so the search tools can sweep it.

![Phys Fuzz, Tokyo '68](products/phys-fuzz/screenshots/companion.png)

The **Circuit** menu above the board picks the pedal: **London '66** (the Fuzz
Face circuit) or **Tokyo '68** (the Shin-Ei Companion FY-2 circuit:
`core/circuit/circuits/ShinEi.h`, a netlist on the general engine mirroring
`sim/shinei/shinei.cir`, which is traced from the schematic in
`products/phys-fuzz/reference/`). Two collector-feedback silicon
stages, the Fuzz pot panning between their collectors, then the passive mid
scoop that makes it thin, gated and 30 dB quieter than the Fuzz Face (made up
in the output scale). The same six knobs drive it through the shared knob
surface: Battery, Temperature and gain fade map directly, Age's bias drift
scales Q2's 1M2 feedback resistor and its junction leak runs over a gentler
range (there is no bypass cap to dry out). Switching circuits warm-starts the
new one at its DC point, so it can be automated. The board redraws as the FY-2
with its own node telemetry; `sim/shinei/compare.py` checks the netlist
against ngspice (bias identical, 2.5-3.6% RMS on the riff, no failures).
Parameter value for `plugin_render`: `"circuit=Tokyo '68"`.

## Iron

![Iron](products/iron/screenshots/blown.png)

A tube line stage (12AU7) into a single-ended, gapped output transformer: the
`sim/tubeout` circuit, verified against ngspice, running as a netlist on the
general engine (stereo, 4x oversampling, ~7x realtime). The knobs describe the
iron: **Drive** (how hard it's hit; output auto-compensated so it changes
colour, not level), **Gap** (DC bias on the core: less gap = fatter, grittier
lows), **Core** (size: smaller saturates sooner), **Steel** (premium to scrap:
more hysteresis), plus Output and a latency-aligned Mix. **Cold start** power-
cycles the stage with a fast supply ramp: a thump, and the core settles with a
different remanent flux.

The editor draws the transformer as its physical core (thickness = Core, slit =
Gap, colour = Steel, brightness = flux) inside the stage schematic, next to the
live B-H loop (zoomed on the loop, with a full-range inset showing where it sits
relative to saturation). Screenshots: `iron_snapshot out.png in.wav --program N`.

Releases: `git tag iron-vX.Y.Z` (the workflow builds whichever product the tag names).

## Solvers

**General engine** (`core/circuit/Circuit.h`, devices in `Devices.h`): describe
a circuit as a netlist (R, C, L, voltage sources, ideal transformers, and
nonlinear devices: BJT, Koren triode, Jiles-Atherton transformer core) and it
derives the realtime DK solver automatically. It reproduces the hand-built
solvers (`build/net_selftest`: transformer to 1e-9, fuzz to 2e-5 RMS) and runs
the tube mic pre in `sim/tubepre` and a single-ended output transformer stage
in `sim/tubeout` and the Shin-Ei FY-2 fuzz in `sim/shinei` (circuits in
`core/circuit/circuits/`, run any by name with `build/circuit_render`). The transformer core device has an air gap and starts
magnetized by any DC it carries. New circuits should be netlists; the
hand-built solvers below stay as references.

Two solvers share one transistor model (`bjt::` in FuzzFace.h):

- **FuzzFaceDK** (what the plugins run): nodal DK method. The linear network is
  folded into small precomputed matrices whenever a knob moves, so each sample
  is a 4x4 Newton solve on the junction voltages plus a few matrix-vector
  products. ~26x realtime at 4x oversampling on desktop, 3.5x the MNA solver.
  `FuzzFaceDKf` is the single-precision version for microcontrollers.
- **FuzzFace**: plain MNA, 11x11 solve per Newton step. The readable reference,
  and it finds the DC operating point for both.

They take identical Newton steps: DK matches MNA to 0.005% RMS, float DK to
~1.5% RMS with the same envelope (0.004 dB) and spectrum (0.02 dB). Compare with
`build/ff_render input.txt out.dat --solver mna|dk|dkf`.

## Hardware (Daisy Seed / Teensy 4)

`tools/embedded/fuzz_m7.cpp` puts `FuzzFaceDKf` behind a C API
(`cd_fuzz_init / cd_fuzz_set / cd_fuzz_process`) for a libDaisy or Teensy Audio
wrapper. `make -C tools/embedded ARM_GCC=<arm-none-eabi bin dir>` builds it for
Cortex-M7 with hardware FPU and checks that the per-sample path has no
double-precision instructions (currently: 0). Not yet run on hardware; the
estimate is roughly a third of a Daisy's CPU at 2x oversampling (96 kHz).

## Changing the circuit

The deck is the spec. `core/circuit/FuzzFace.h` (and so FuzzFaceDK.h) mirrors its topology, values and
transistor model by hand, so change both together and re-run `compare.py`.
