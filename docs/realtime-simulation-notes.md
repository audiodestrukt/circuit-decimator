# Realtime physical simulation: what we've learned

A running log of the techniques, traps and measurements that decide whether a
physical model runs in realtime *and* stays true to the physics. Add to it
whenever something is learned the hard way. Each entry says what we did, why it
works, how much it bought (measured), and how we know it didn't change the
answer.

Numbers are from the i9-14900K workstation (`tools/perf_bench`, `tests/perf_baseline.json`)
unless noted. "% of a core" means one channel at a 48 kHz host rate.

---

## 0. Ground rules

1. **Exact reference first, fast version second.** Every model has an offline
   reference it must match: an ngspice deck (`sim/*/*.cir`), an analytic formula,
   or scipy. The realtime version is only allowed to differ by a *measured* error.
   Nothing gets approximated without a number next to it.
2. **Fidelity budget.** Spend CPU where the physics is audible or unique (tube
   clipping, grid blocking, iron, the speaker as the amp's load, cone breakup at
   the mic). Approximate where it isn't: line-level transformers, the room, the
   mic body. See `docs/briefs/2026-09-27-simulation-roadmap.md` §2.
3. **ngspice is the reference, never the engine.** On the Fuzz Face deck,
   ngspice ran at ~0.8–1.6x realtime at 4x oversampling. The first custom MNA
   solver ran at 7–10x, and the DK version at ~26x, matching MNA to 0.005 % RMS.
   A fixed-step solver built for the circuit wins by an order of magnitude, and
   ngspice's variable step is exactly what you don't want in an audio callback.
4. **Measure cost at the rates people actually run.** Test 44.1/48 kHz *and*
   96/192 kHz. Costs that scale with rate² stay hidden at 48 kHz (§4.3).

---

## 1. Solver tricks (core/circuit/Circuit.h)

### 1.1 Newton only on the nonlinear ports (DK / MNA reduction)
Fold the linear network into matrices once. Newton then iterates only over the
nonlinear device ports: a handful of unknowns instead of every node. On the Fuzz
Face this gave 3.5x over the full-MNA Newton solver, with identical results
(same Newton steps, smaller system).

### 1.2 Extrapolated initial guess
Start each sample's Newton from a linear extrapolation of the last two samples'
port voltages, not from the last value. Iterations fall on smooth signals, and
the result doesn't change (Newton converges to the same point).

### 1.3 Block-diagonal Jacobian
When the nonlinear ports split into groups that don't share unknowns, solve each
block separately. The cost is Σ(nᵢ³) instead of (Σnᵢ)³.

### 1.4 "Linear within the sample": one Newton step
`Device::linearWithinSample()`. Some devices are nonlinear in *state*, but
linear in the current sample's unknowns once that state is frozen. The speaker
motor is the example: Bl(x) depends on displacement x, which is integrated from
velocity and only moves in `commit()`. If every device in a circuit says so,
Newton's first step is exact and the loop breaks after it. It is skipped for DC
solves, and when a step was limited.

- **Cost:** the nonlinear speaker went from 295 to 201 ns/sample, together with §1.5–1.7.
- **Accuracy:** a one-sample lag in the state, the same semi-implicit
  treatment a displacement integrator already has. Against ngspice's fully
  implicit solve at 96 kHz, displacement is identical, THD agrees within 0.2 %,
  compression is identical, and the waveform error is ≤ 2.7e-3 (`sim/speaker/nonlinear.py`).

### 1.5 Slow parameters through a device port, not a matrix rebuild
Coil heating changes Re by up to ~20 % over seconds. Changing a resistor's value
forces a matrix rebuild (refactorisation). Instead, keep the resistor at its
cold value and add the difference as a conductance on a device port:
`gh = 1/re - 1/re0`. That costs one extra port evaluation per sample, and no
rebuild ever.

This generalises to anything that drifts slowly: tube heater sag, bias drift,
temperature, component ageing.

### 1.6 Tabulate the expensive function, keep the exact one for the table
Bl(x) is an overlap integral of the coil with a tanh-fringed gap field (logs and
exps). It is tabulated at 2049 points over ±12 mm and rebuilt only when gap,
Xmax or fringing change. The exact function stays in the code and builds the table.

### 1.7 Compute state-dependent quantities once per sample, in `commit()`
Evaluate Bl(x) when x updates (once per sample), not inside `eval()`, which
runs every Newton iteration.

### 1.8 Slow physics at a slow rate
- **Coil heating** is two thermal RC stages (coil at 10 s, magnet at 20 min),
  stepped every 256 samples with explicit Euler. That is stable by a huge margin,
  and it is negligible cost.
- **LA-2A sidechain** at a quarter of the audio rate stays within 0.2 dB of the full-rate solve.

Rule: step a subsystem at a rate matched to its fastest time constant, not the audio rate.

---

## 2. Partitioning (splitting a circuit)

- **LA-2A:** the audio path and the sidechain are solved as two circuits that
  meet only at one node and the T4 cell, with a one-sample exchange. That gave
  about 3x, against ngspice solving it as a single circuit. Envelopes agree to
  0.00 dB RMS and 0.09 dB max, and the waveform error is ≤ 4.7e-4 (`sim/la2a/README.md`).
- **Why it works:** solve cost grows with the cube of the coupled ports, so
  cutting where one side barely loads the other pays off superlinearly.
- **Good cut points:** a cathode follower's output, a grid behind a coupling
  cap, a supply rail, an optical coupling. Always measure the cut against the
  unsplit netlist.
- **Next:** automatic partitioning (roadmap §3). Not done yet.

---

## 3. Oversampling

- **Oversample only what makes aliasing.** Hard-clipping stages need 4x; a
  linear system gains nothing from it. The Bench used to run the (linear) cab at
  4x like every circuit. Per-circuit oversampling (`BenchCircuit::oversamplingLog2()`)
  runs it at 1x:
  - 96 kHz host: 18 % → 0.8 %;
  - 192 kHz host: 73 % → 1.5 %.
- **The nonlinear speaker still runs at 1x.** Its nonlinearity acts on
  displacement: a low-passed signal that barely has content above a few hundred Hz.
  - The harmonics it makes sit far below Nyquist.
  - Aliasing isn't the concern here, as it is with a clipping transistor.
  - Check this again if a stage with fast nonlinearity (clipping) ever joins the speaker circuit.
- **Plan:** per-partition rates. Clipping stages run oversampled, linear parts
  at the base rate, supplies below it.

---

## 4. The hybrid "physics → impulse response" model (core/acoustic/CabModel.h)

### 4.1 Split time-varying from time-invariant
The speaker is two very different problems:
- **Driver and box:** a small lumped circuit. It is nonlinear at volume, it is
  the amp's load, and it must run per sample.
- **Cone breakup, radiation, mic:** expensive to compute (a shell FE model, a
  Rayleigh integral over the cone, the mic's pattern and response), but **linear
  and time-invariant for a given knob setting**.

So the second part is computed off the audio thread as an impulse response and
convolved with the circuit's cone velocity. The expensive physics costs nothing
per sample; it costs 2–105 ms per knob move, on a worker thread.

This is the pattern for anything big and linear: rooms, cabinets, box modes, and
eventually the non-axisymmetric cone.

### 4.2 Staged caching of the IR build
The build has stages: cone FE, then coupling, then radiation, then mic. Each
stage caches its result and depends on a range of parameters (`CabParam` order
matters). Moving a mic knob redoes only the mic stage, so the rebuild is cheap
exactly where the user moves knobs the most.

### 4.3 Convolution cost can scale with rate²
We saw a mystery: 25 % CPU in the Bench, the same at every buffer size.
- **Cause:**
  - The IR length in samples grows with the rate, and was rounded up to a power of two.
  - The oversampling multiplied the rate again.
  - So the cost grew roughly with rate², and at 4x of 48 kHz it was huge.
- **Clue:** "doesn't depend on buffer size" means per-sample work, not per-callback overhead.
- **Fixes:**
  - Trim the IR to its physical length (45 ms), with a 10 % fade-out.
  - Scale the convolution's partition size with the rate (64 at 48 kHz).
  - Run the cab at 1x (§3).

### 4.4 Lock-free IR handover
- The worker builds the new IR spectra.
- The audio thread swaps them in with one atomic exchange, when the convolver can accept.
- The old spectra go back through a "trash" slot for the worker to free.
- Nothing allocates, locks or frees on the audio thread.

### 4.5 Correct the lumped circuit with the distributed physics
The lumped circuit's cone velocity is corrected by a filter from the FE model's
neck impedance and the piston radiation impedance (Struve H1), in `Coupling.h`.
The circuit stays cheap, and the effect of breakup on the drive point is not lost.

---

## 5. Accuracy traps we fell into

| Trap | Symptom | Fix |
|---|---|---|
| A measured magnitude response applied with zero phase | acausal IR; 4.9 dB error at 100 Hz | build minimum phase from the magnitude (cepstrum), `Radiation.h minimumPhase` |
| One acoustic path skipped a stage the others had | the open back's rear wave *reinforced* instead of cancelling | every path (front, rear, reflection) goes through the same mic pattern and response |
| An idealised boundary (infinite baffle) | the open back was louder than closed at 1 m | model the finite baffle's edges (edge-diffraction sources) |
| Knob changes updated some circuit elements but not all | the back-opening knob did nothing to the audio | one `retune()` that sets **every** element from the parameters; no partial updates |
| A stale tool binary | a 0.18 dB "regression" blamed on new device code | rebuild before verifying; better, make the check depend on the build (ctest) |
| Summing all audio inputs on live input | clipping in bypass, phasiness | pick one channel (the loudest, with hysteresis) |
| Letting the device choose odd rates (64 kHz) | pitch shift | pin/snap to standard rates |

**Portability traps:**
- MSVC has no `M_PI` without `_USE_MATH_DEFINES`.
- libc++ (macOS) has no `std::cyl_bessel_j`. We ship our own `Bessel.h`, within 3e-9 of scipy.

The release CI caught both. Build all three platforms before calling a feature done.

---

## 6. Measuring CPU so regressions get caught (tests/)

- **Use thread CPU time, not wall time.** Take the best of N runs, and normalise
  by a reference FFT workload timed in the same run. That makes numbers
  comparable across load and clock changes.
- **Count Newton iterations.** They are a deterministic signal, independent of
  the machine. A jump in iterations is a solver regression even when timing noise hides it.
- **Keep per-machine baselines** (`tests/perf_baseline.json`). CI has its own machine name.
- **Run the timing test serially** (`RUN_SERIAL` in ctest). Timing next to other tests is meaningless.
- **The test catches intentional costs too.** It flagged the nonlinear speaker
  (+119–125 %). That is correct behaviour.
  - First try to make the new work cheap (§1.4–1.7 took it from 295 to 201 ns/sample).
  - Then update the baseline on purpose, with the reason in the commit.

---

## 7. Speaker large-signal model: the specifics

Recorded because the same patterns will come back in the amp (tubes, iron).

- **Bl(x):** an overhung coil (gap 7.9 mm, overhang Xmax 0.48 mm) moving through
  a gap field with tanh fringing (1 mm). Bl is the overlap integral, in closed
  form via G(u) = f·(|u|/f + log1p(e^(−2|u|/f)) − ln 2).
  - Bl falls to 82 % at 2.15 mm. The datasheet Xmax (coil overhang) is not where it "runs out".
- **Suspension:** k(x) = k0 (1 + (x/Xs)²), which adds a cubic restoring force.
- **In the mobility analogy:** Bl and Sd are ideal transformers. The motor
  device adds the corrections as currents on four ports: the electrical side, the
  mechanical side, the suspension, and the heating conductance.
- **Result:**
  - At 50 Hz and 40 V: 48 % THD, −4.8 dB compression.
  - At 2 V: essentially linear. It stays linear until the cone gets past Xmax.
- **Heating check** (time constants shortened):
  - The coil rise matched the steady state P·(Rcoil + Rmagnet) (55.58 vs 55.56 K).
  - Re went from 6.40 to 7.80 Ω.
  - The drop in current matched an independent |Z| calculation: −0.90 vs −0.96 dB.
  - The 0.06 dB gap comes from peak detection vs the steady-state phasor, and Le's share of |Z|.
- **Cost:** 91 → 201 ns/sample at 192 kHz, about 1 % of a core at 48 kHz.
- **Plugin mapping:** Cone's Power knob sets volts at full scale (peak of a sine
  of that power into 8 Ω). The output gain divides by the same volts, so turning
  Power up changes the *character* (excursion, compression), not the loudness.

---

## 8. Tricks planned but not proven yet

Move each one up into the sections above once it's measured.

- **Linear subsystem extraction:** tone stacks and passive EQ as state-space/IIR filters from the same matrices. No Newton.
- **Automatic partitioning** with a one-sample exchange at the cuts, and each cut's error measured.
- **Supply sag at a decimated rate** (ms time constants).
- **Skip grid-conduction evaluation** when the grid is far below cutoff.
- **Tabulated Koren curves** in the realtime path, with exact curves in the reference build.
- **Multi-driver cab:** identical drivers fed the same signal move identically, so sum the per-driver IRs into one IR per mic. That means one convolution per mic, not per driver-mic pair.
- **Faster mic stage rebuild:** a shorter FFT, then interpolate to the final grid (~8x expected).
- **Speaker breakup impedance** as a short fitted IIR on the amp's load, refreshed on knob changes.
