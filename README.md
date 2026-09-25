# Circuit Decimator

A VST3 fuzz whose "knobs" are component failures: a realtime circuit simulation
of a Fuzz Face where you can starve the battery, leak the junctions, cook the
transistors and automate all of it.

A monorepo: one shared circuit core, one folder per product.

```
core/circuit/        FuzzFaceDK.h (realtime solver: nodal DK method, double or float, no JUCE/heap)
                     FuzzFace.h (reference MNA solver + shared transistor model; DC operating point)
                     Knobs.h (full control surface: ranges/curves/presets -> circuit values)
core/engine/         FuzzEngine: the shared audio path (4x oversampling, smoothing, telemetry)
core/ui/             CircuitView: live schematic (node voltages, signal glow, damage in red)
products/workbench/  Circuit Decimator plugin (every component knob) + Circuit Bench app
products/phys-fuzz/  Phys Fuzz plugin: Battery / Age / Temperature macros over the core
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

The schematic is live: wire colour is each node's voltage (blue 0 V to amber
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

Six knobs: Fuzz, Volume, **Battery** (charge: voltage falls and sag climbs,
0% is tuned to sputter, not die), **Age** (junction leak, dried caps, fading
gain, cold bias drift), Temperature, Output. The mapping lives in
`products/phys-fuzz/Macros.h` and is a first pass for tuning by ear; it's also
exposed through libfuzzface so the search tools can sweep it.

## Solvers

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
