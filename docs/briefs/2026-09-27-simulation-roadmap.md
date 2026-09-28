## Simulation roadmap: amp, multi-driver cab, preamps (2026-09-27)
- **Goal:** keep building the simulation workshop: a full guitar amp, cabs with several drivers and mics, and more preamps. Be exact where it's interesting or audible, and approximate the rest for CPU.
- **Done looks like:** a whole rig (amp → speaker cab → mics), physically modelled end to end, at a CPU cost a player can run several of. Every approximation has an exact reference in the repo and a measured error.
- **Not now:** room simulation beyond simple early reflections; Daisy/Teensy; bass amps and exotic topologies; a product UI for the whole rig (the Bench first).
- **First slice:** the engine's partitioning, the enabler for everything below. Then the amp's power section, driving the Cone speaker's real impedance, which nothing else does.
- **Open question:** how far partitioning and per-stage oversampling bring a full amp's CPU down (target: under ~8% of a core per channel at 48 kHz) before the sound changes.

---

## 1. Where we are

Per channel, share of one core at a 48 kHz host rate (`tools/perf_bench`, i9-14900K):

| Model | How it runs | Cost |
|---|---|---|
| Speaker cab (Cone) | host rate; circuit + partitioned convolution; IR rebuilt off-thread (2–105 ms per knob move) | ~1.3 % |
| Fuzz, London '66 | hand-built DK, 4x oversampled | ~2.9 % |
| Fuzz, Tokyo '68 | netlist, 4x | ~4.2 % |
| SE output stage (Iron) | netlist, 1 triode + JA core, 4x | ~4.5 % |
| LA-2A (Opto) | netlist split in two (audio + sidechain), 2x | ~12 % |

A full guitar amp built the naive way would be three preamp triodes, a two-triode phase inverter, two power pentodes, a push-pull output transformer with hysteresis, global feedback and a supply: roughly 20 nonlinear ports in one netlist at 4x. That's about LA-2A-before-splitting territory: **~40–60 % of a core**. The plan exists to fix that.

## 2. The fidelity budget

Rule: **physics where it's audible or unique; approximate where it isn't; always keep the exact version as the reference** (as ngspice is for the circuits) and measure the error.

| Part | Why it matters | Keep exact? | Approximation, if any | CPU effect |
|---|---|---|---|---|
| Tube stages clipping, bias shift, blocking (grid current charging coupling caps) | the sound of overdrive, touch response | **exact** | — | the main cost |
| Output transformer core (hysteresis, saturation, DC bias) | low-end compression, "iron" | **exact** at power-amp levels | a linear core when the drive is low (auto-switch, or a "tolerance" knob) | JA ~ one port |
| Input/interstage transformers at line level | barely leave the linear region | approximate | linear L/R/C (exact within its range) | −1 port each |
| Tone stacks, passive EQ, coupling networks between stages | linear: exact in any form | exact **and** cheap | extracted from the netlist as a linear filter (same math, no Newton) | large |
| Power supply sag (rectifier, filter caps, B+ droop) | feel and compression, unique | **exact** | solved at a decimated rate (supply time constants are ms) | small at 1/8 rate |
| Speaker as the amp's load (impedance: resonance, coil inductance, breakup bumps) | amp–speaker interaction: **unique**; IR cab sims can't do it | **exact** (the lumped part + the cone's neck impedance) | the breakup part of the impedance as a short IIR fit, refreshed on knob changes | small |
| Speaker cone breakup, near-field mic | the cab's voice and mic placement | exact (in the IR build) | IR build off-thread | none on the audio thread |
| Uneven (non-axisymmetric) cone, damage | small-move mic sensitivity, blown speakers: **unique** | exact in the IR build | — | IR build only |
| Motor nonlinearity (Bl(x), Cms(x), coil heating) | speaker "breakup" at volume, power compression | exact | per sample, but only the motor (a few ports) | small |
| Mic body reflection | ±0.5–1 dB | approximate | already a simple disc formula | none |
| Room | at close range, 30 dB down | approximate | a few image-source early reflections + an optional short tail | small |
| Oversampling | aliasing from hard clipping | per stage | only the stages that clip, at their own factor; linear parts at the base rate | large |

## 3. Engine work (the enabler)

1. **Automatic partitioning.** The LA-2A split its sidechain by hand and got ~3x. Generalise it: split a netlist where one side barely loads the other, and exchange voltages across the cut with a one-sample delay:
   - a cathode follower's output;
   - a grid behind a coupling cap, where grid current is negligible until it isn't;
   - a supply rail, at the supply's own rate.

   Measure each cut's error against the unsplit netlist. Solve cost falls as Σ(ports³) rather than (Σ ports)³.
2. **Linear subsystem extraction.** A purely linear chunk of the netlist (tone stack, EQ, passive networks) is solved as a state-space / IIR filter derived from the same matrices. No Newton, same answer.
3. **Per-partition rates.** Clipping stages oversampled; linear parts and the supply at the base rate or below; resampling at the cuts. Oversampling cost is paid only where aliasing comes from.
4. **Cheaper devices.** Skip grid-conduction evaluation when the grid is far below cutoff, and use tabulated Koren curves where Newton doesn't need exact derivatives. Keep the exact versions for the reference build.
5. **CPU tests grow with it.** Each new model gets a `perf_bench` entry and a baseline; each approximation gets an accuracy test against its exact reference, as `cab_accuracy` does now.

## 4. Amp sim

A classic guitar amp as one physical netlist, stage by stage. Each stage gets an ngspice deck first, as every circuit so far has. Circuits are named by origin rather than trademark, as the Phys Fuzz circuits are.

1. **Preamp:** 2–3 12AX7 stages with cathode bypass, a bright cap and interstage coupling caps. Grid current and blocking distortion stay exact, since they are the character.
2. **Tone stack:** the passive bass/mid/treble network, extracted as a linear filter; its knobs move its coefficients.
3. **Phase inverter:** a long-tailed pair (two triodes). This is where much of the "power amp" feel starts to distort.
4. **Power stage:** push-pull pentodes (6L6/EL34 class, the 6AQ5 model generalised) into a **push-pull output transformer**. It needs a new core element: a JA core with a centre-tapped primary and a secondary, with the two halves' DC cancelling. Includes screen supply, bias and crossover distortion.
5. **Global negative feedback and presence:** from the OT secondary back to the phase inverter.
6. **Power supply:** rectifier (solid-state or tube, with its internal resistance), filter caps, sag. It runs at a decimated rate as its own partition.
7. **The speaker as the load:** the Cone driver's electrical impedance on the OT secondary: the lumped circuit per sample, plus a short fitted filter for the breakup bumps. Then the cab's mic IR, driven by the *actual* cone velocity from that coupled solve. This is the part that's unique.

**Verification:**
- each stage and the whole amp against ngspice;
- power output and clipping onset against published figures for the topology;
- the speaker load's effect: output voltage vs frequency into the modelled speaker, against a resistive load (the classic "speaker impedance hump" measurement).

**CPU target:** under ~8 % of a core per channel at 48 kHz, after partitioning.

## 5. Speaker cab: multiple drivers and mics

- **Layouts:** 1x12, 2x12, 4x12, 2x10, 4x10 on a sized baffle. Each driver has its own position; mics see every driver with its own delay and angle (the Rayleigh integral per driver).
- **Shared box:** drivers in one enclosure share its air, so each sees the box compliance scaled by the number of drivers. The lumped circuit gets N drivers on one box node.
- **Cost trick:** identical drivers fed the same signal move identically, so a mic's IR is the **sum** of the per-driver IRs: one convolution per mic, not per driver-mic pair. Driver-to-driver tolerance (slightly different Fs, Qts, cone) is an option: one circuit per distinct driver.
- **Multiple mics:** 2–3 mics, each with its own position, angle and type, mixed with level and polarity per mic. Time alignment is optional: the physical delay between mics is part of the sound (the comb when they're mixed).
- **Box standing waves:** the interior's modes (dimensions → frequencies, stuffing → damping), coupled to the rear of the cones and leaking through the back or the panels. Modal filters in the IR build.
- **Uneven cone and damage** (§2): the non-axisymmetric shell (circumferential orders 1–4), excited by asymmetry (glue, seams, a hole, an off-centre coil). This is the biggest remaining accuracy gap (small mic moves), and it's also the path to holes, burns and tears.
- **The IR tuning loop:** fit the physical parameters to real cab IRs with known mic positions. It tests placement accuracy and gives "explain / morph / edit a real capture".
- **Mic rebuild speed:** 105 ms now. A shorter FFT for the mic stage and interpolation to the final grid should give ~8x, and matter more with several mics.

**Cost:** per mic, one convolution plus the per-driver circuits. A 4x12 with two mics is ~3 % of a core.

## 6. Preamps (mic / console)

The mic transformer and tube mic pre exist in the Bench. Candidates:
- **Transformer-coupled class-A transistor pre:** input transformer (JA), two or three discrete gain stages, output transformer. The "console" sound.
- **Tube channel strip:** extend TubePre with a make-up stage and an output transformer.
- **Op-amp preamp:** macro-modelled op-amps, for the clean-modern end.

Each gets ngspice decks and Bench entries first; products later.

## 7. Sequencing

1. **Engine partitioning + linear extraction** (§3). Proven by re-splitting the LA-2A automatically and matching the hand split, and by the SE stage.
2. **Push-pull OT core + power stage + supply** (§4.4–4.6), verified stage by stage.
3. **Speaker as the load** (§4.7): the power stage driving Cone's driver, with the coupled cone velocity into the mic IR. First milestone that's unique: "amp + speaker, physically coupled" in the Bench.
4. **Preamp, tone stack, phase inverter, feedback** (§4.1–4.3, 4.5) → a whole amp in the Bench, with a CPU check.
5. **Multi-driver + multi-mic cab** (§5), then box modes.
6. **Uneven cone + damage**, and the IR tuning loop when real IRs arrive.
7. **Preamps** (§6) in between, as smaller self-contained pieces.

Products come out of the Bench once each piece is verified and within its CPU budget: an amp, a bigger Cone (multi-driver, multi-mic, damage), and a preamp.
