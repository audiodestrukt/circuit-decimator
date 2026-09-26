## Speaker cabinet simulator: kickoff brief (2026-09-26)
- **Problem:** Dialling in a guitar cab today means flipping through IRs someone else captured. Instead, change the physical things: the driver, the box, the mic, and where the mic sits. Then hear what that does.
- **Done looks like:** a VST with one 12" driver in a closed-back or open-back box.
  - **Driver knobs:** Thiele-Small parameters, cone size and cone stiffness/material.
  - **Box knobs:** volume, dimensions, open or closed back.
  - **Mic:** one mic you can move across the cone, pull back and angle, with a choice of capsule size.
  - It sounds like a mic'd cab, and each knob moves the sound the way the physics says it should.
- **Not now:**
  - Amp–speaker interaction. The input is the voltage from a low-impedance (solid-state) amp; driving it from Iron's output transformer comes later.
  - Multi-driver cabs, the room, more than one mic.
  - Aging and damage: holes in the cone, a partly burnt voice coil. These are pass 2, but the architecture must leave room for them (see [Damage later](#damage-later-pass-2)).
- **Approach:** a hybrid, "physics → IR".
  - The driver's motor and the box run as a nonlinear netlist circuit on the existing engine, per sample.
  - A physical model of the cone, the cabinet and the mic builds an impulse response. It's rebuilt whenever a knob moves, and the signal is convolved with it.
  - The alternatives are compared below.
- **First slice:**
  1. The driver and box as a netlist, verified against ngspice and textbook closed-box theory.
  2. The rigid-piston-to-mic path.
  3. Cone breakup.
  4. Real cab IRs as the accuracy check.
- **Open question:** whether a cone model built from rings and bending modes reproduces real 12" breakup well enough to sound real. That means the 2–5 kHz peaks and the comb-like changes as the mic moves across the cone.

---

## 1. What a mic'd guitar cab physically does

The signal passes through four physical domains. Each has a well-known equivalent circuit up to some frequency, and that frequency is where the modelling gets hard.

| Domain | What's there | Circuit analog | Valid up to |
|---|---|---|---|
| Electrical | Voice coil resistance Re and inductance Le. Le is lossy: eddy currents in the pole make it a "semi-inductor", usually modelled as Le plus a parallel L2/R2. | R, L | all audio |
| Motor | Force = Bl·i; back-EMF = Bl·velocity. Bl falls as the coil leaves the gap. | gyrator (Bl), later Bl(x) | all audio |
| Mechanical | Moving mass Mms, suspension compliance Cms (stiffens near Xmax), losses Rms. | L, C, R (impedance analogy: force ↔ voltage, velocity ↔ current) | until the cone breaks up (~1 kHz for a 12" paper cone) |
| Acoustic | The cone area Sd couples to the air. Box air is a compliance Cab = Vb/(ρc²). An open back is an acoustic mass plus a leak. Radiation: air mass loading plus a resistance that grows with f². | transformer (Sd), C, L, R | until standing waves in the box (~λ/2 = the box's longest dimension, a few hundred Hz) |

Above those limits sits what guitarists actually hear:

- **Cone breakup.** Above about 1 kHz the paper cone stops moving as one piece. Bending waves ring across it: concentric (rings) and radial (lobes) modes. The cone's shape, paper stiffness and density, the dust cap and the surround set their frequencies and damping. This is the 2–5 kHz "voice" of a speaker model, and the reason a V30 and a Greenback differ.
- **Radiation to the mic.** The mic is 1–10 cm from a 30 cm radiator, so it sits in the **near field**. Every part of the cone arrives with a different delay, and different cone regions are moving in different phases above breakup. Moving the mic from the dust cap to the edge changes what it hears drastically, and that's the effect the mic knobs exist for. The correct tool is the Rayleigh integral over the vibrating surface.
- **Cabinet.**
  - Closed back: internal standing waves between the panels, damped by stuffing; panel resonances.
  - Open back: the rear wave comes round the baffle (a dipole) and cancels the bass. Edge diffraction makes ripples.
- **Mic.**
  - Capsule size averages the pressure over the diaphragm, so a large capsule loses highs, more so off-axis.
  - Polar pattern: a cardioid is a pressure and pressure-gradient mix.
  - **Proximity effect:** the gradient part boosts bass roughly as 1/(kr) up close.
  - The capsule's own resonance, e.g. the classic dynamic presence peak.
- **Nonlinearities**, all in the motor and suspension, which is why they stay per sample:
  - Bl(x) and Cms(x): excursion compression and asymmetric distortion near Xmax.
  - Voice-coil heating: Re rises with temperature, giving power compression over seconds.
  - Paper-cone nonlinearity at breakup: real, but hard. See open questions.

Acoustics at these levels are linear. Everything from the cone's motion onwards is a linear, position-dependent filter. That fact decides the architecture.

## 2. Architectures considered

### A. Measured IR (what everyone ships)
Convolve a captured cab IR.
- **Plus:** exact for that one cab, mic and position. Cheap.
- **Minus:** no physics. A new sound means a new capture. No nonlinearity, no damage, no continuous mic moves (only crossfades between captured positions).
- **Role here:** evaluation only. Real IRs measure how accurate our model is. They aren't the product.

### B. Lumped circuit only (Thiele-Small in the netlist engine)
The electro-mechano-acoustic circuit from section 1, per sample, output = on-axis far-field pressure of a rigid piston.
- **Plus:** exact where it's valid. Nonlinear for free. Fits the engine and verifies against ngspice like everything else. Automatable, with no rebuild latency.
- **Minus:** correct only below breakup. It misses cone breakup, the near field, mic position and box modes: everything above ~1 kHz, which is what makes a cab sound like a cab.
- **Role here:** the low-frequency and nonlinear core of the chosen design (D).

### C. All modal, all per sample
Every cone bending mode and cabinet mode becomes a resonator in the per-sample simulation, driven by the motor force. Each mode's output is weighted by its radiation toward the mic: position, angle, capsule. Sum the weighted outputs.
- **Plus:** one time-domain system, so the motor's nonlinearity acts on everything. No convolution latency or rebuilds. Mic moves only change weights, so automation is smooth.
- **Minus:** cost. A 12" cone needs ~50–200 modes to 8 kHz, plus box modes. That's affordable (a few µs per sample) but heavier than an IR.
  - A mode's radiation to a near-field mic isn't one gain: it's a filter, because different parts of the mode shape arrive with different delays. Treating it as a gain smears exactly the comb effects that make mic placement interesting.
  - Mode counts rise fast with bandwidth.
- **Role here:** it lives on as variant D3 (modal weights for mic automation) if D's rebuild-on-change proves too coarse.

### D. Hybrid "physics → IR" (chosen)
Per sample, the nonlinear netlist runs: amp voltage → voice coil → motor → cone as a piston → box air. Its output is the **cone's piston velocity**, or equivalently the force at the voice coil. A physical model of the cone, the cabinet and the mic turns that into an impulse response. The IR is rebuilt on a background thread whenever a knob changes, then crossfaded in. The audio thread convolves the piston velocity with it.

The design questions are **where to cut** between the per-sample part and the IR, and **what form the IR part takes**. These are the "partial IR" modes.

#### Where to cut

| Cut at | Per sample (nonlinear) | In the IR (linear) | Trade-off |
|---|---|---|---|
| **Voice-coil force** (recommended) | amp → Re/Le → motor, with Bl(x) and heating. The piston and box circuit also runs per sample, because excursion needs x. | the cone's full mechanical and modal response to force at the neck, then radiation, box and mic | The breakup modes are driven the way they physically are: by force at the neck. The IR's low end must exclude the piston resonance already simulated, or divide it out. |
| **Piston velocity** | everything up to and including the piston and the box air | breakup as a ratio to piston motion, radiation, box modes, mic | Simplest seam. Loses the breakup modes' reaction on the motor: the small impedance ripples above 1 kHz. Close enough for v1. |
| **Band split** | the lumped model below ~500 Hz, output directly | an IR above ~500 Hz, driven by the same input | Easy to reason about. Needs a crossover whose phase has to match physically. The seam can be audible when mic moves change the low end. |

Either the force or the velocity cut works for v1. They differ only in whether the breakup modes' load on the motor is kept. Build the velocity cut first, and keep the IR builder's interface as "transfer from the cut quantity to mic pressure" so moving to the force cut is local.

#### What form the IR part takes

- **D1: one rebuilt FIR (the baseline).** Build the full transfer on a frequency grid from the physics: cone modes × Rayleigh integral to the mic × capsule averaging × box modes. Take the inverse FFT to get an IR of ~20–80 ms, and run uniformly partitioned convolution (zero or low latency). A rebuild takes milliseconds; crossfade old to new over ~20 ms.
  - Knob moves are smooth enough for turning; automation is fine but not sample-accurate.
- **D2: split IR, a static part and a moving part.** Things that change rarely (cone material, box) fold into one IR. Mic-dependent parts are a second, short IR (or filter) rebuilt more often. Mic moves then rebuild only the cheap part.
- **D3: modal weights for the mic.** For each cone mode, precompute its pressure at the mic as a short filter (a few taps, keeping the near-field delay spread) on a grid of mic positions and angles. Moving the mic interpolates weights instead of rebuilding. This gives sample-smooth mic automation (sweeps, "mic falling off the grille" effects) at modal cost for the mic-dependent part.
- **D4: fitted parametric filters.** Fit the physics-built response with a bank of biquads (peaks at the mode frequencies, shelves for the capsule and proximity). Cheapest, perfectly smooth. But it loses the fine near-field comb structure, which is what placement sounds like. Keep it as a CPU fallback.

**Recommendation:** D with the velocity cut and D1 for the first pass. Add D2 when mic moves feel sluggish, and D3 if automated mic sweeps become a feature.

### E. Offline field solver → IR
An axisymmetric finite-difference (FDTD) or boundary-element solve of the real geometry (cone, dust cap, baffle, box, air), run when a parameter changes, producing the IR.
- **Plus:** the most physical: diffraction, cone shape and baffle edges all come for free.
- **Minus:** seconds per change, not milliseconds. It's a serious build (meshing, absorbing boundaries, coupling the cone's structural modes to the air). It never feels like a knob.
- **Role here:** **the acoustic reference, the ngspice of this product.** An offline axisymmetric FDTD of a vibrating cone in a baffle is the check for D's Rayleigh integral and modal cone. It can be slow and simple because it only runs in verification.

### F. Learned model (neural net conditioned on parameters)
Train on many IRs, conditioned on driver, box and mic parameters.
- **Minus:** no physics. It can't extrapolate to a hole in the cone, and needs a dataset we don't have. It's the opposite of what this product is for. **Rejected.**

### Comparison

| | Low-end accuracy | Breakup and mic placement | Nonlinear | CPU | Knob response | Damage modelling | Effort |
|---|---|---|---|---|---|---|---|
| A. Measured IR | exact, one cab | exact, one cab | no | low | none: swap IRs | no | none |
| B. Lumped only | exact | no | yes | very low | instant | motor only | low |
| C. All modal | exact | approximate (gains) | yes, everywhere | medium | instant | yes | medium |
| **D. Hybrid** | **exact** | **yes** | **motor yes, cone no** | **low–medium** | **~20 ms crossfade** (D1); instant for mic (D3) | **yes** | **medium** |
| E. Field solver | exact | best | no | offline | seconds | yes | high |
| F. Learned | trained range only | trained range only | maybe | medium | instant | no | high, plus data |

## 3. The physical model inside D

- **Driver circuit** (per sample, netlist engine): Re, Le with L2/R2, Bl as a gyrator, Mms/Cms/Rms, Sd as a transformer.
  - Box: closed = Cab plus losses. Open back = acoustic mass and resistance for the opening, and the rear radiation path.
  - Radiation impedance of a baffled piston.
  - Later: Bl(x), Cms(x), and a thermal network (voice coil → magnet → air) moving Re.
  - Knobs: T/S parameters, or physical ones (cone mass, surround stiffness, magnet strength) mapped to T/S.
- **Cone:** an axisymmetric conical shell. Knobs: diameter, cone angle, paper thickness, stiffness (Young's modulus), density, loss factor, dust cap, surround.
  - First pass: analytic or semi-analytic bending modes of a conical shell, rings × radial orders, each with a frequency, damping and mode shape.
  - Later, if analytic modes fall short: a small finite-element eigen-solve on a 1D axisymmetric mesh, run when knobs change (milliseconds).
- **Radiation:** discretise the cone and dust cap into ring elements (and angular segments for radial modes). Sum each element's contribution with its true distance and delay to the capsule. That's the Rayleigh integral with a baffled piston, valid in the near field.
  - Mic position (across the cone and distance) and angle enter through those distances and the capsule's orientation.
- **Mic capsule:**
  - Diaphragm size: average the pressure over its area, which rolls off highs off-axis.
  - Polar pattern: an omni/figure-8 mix, where the gradient part adds proximity effect from the true distance.
  - Diaphragm resonance: a frequency and a Q, the "presence peak".
- **Cabinet:**
  - Closed: rectangular-room modes of the box interior (dimensions → mode frequencies, stuffing → damping), coupled to the cone's rear face.
  - Open back: the rear wave delayed and diffracted round the baffle edges, as seen at the mic.
  - Panel resonances: later.

## 4. Damage later (pass 2)

The architecture has to make these local changes, not rewrites. With the hybrid, each has a home.

| Damage | Physics | Where it lives in D |
|---|---|---|
| Hole(s) in the cone | Less radiating area. An acoustic leak front-to-back through the hole (acoustic mass plus resistance), giving a bass loss and a whistle. Mass removed, so mode frequencies shift. Asymmetry excites modes a symmetric cone doesn't. | Leak: the netlist, as an acoustic branch. Area and modes: the cone model, with an angular term that breaks symmetry. Radiation: holes are missing ring segments, and the hole itself radiates. |
| Torn or loose surround | Lower, lossier Cms. Rocking modes. | Netlist (Cms, Rms). Cone model (edge boundary condition). |
| Voice coil partly burnt | Re up (damaged turns). Bl down (fewer active turns). Shorted turns act like a shorted secondary, changing Le/L2. A deformed coil rubs: friction, buzz. | All in the per-sample netlist. Rub is a nonlinear friction device, which is why the motor stays per sample. |
| Dried or aged paper | Stiffer, lossier cone. Breakup moves up and gets peakier. | Cone material knobs. |

## 5. Verification

The ngspice pattern, extended to acoustics:
- **Lumped part:** an ngspice deck of the electro-mechano-acoustic circuit (same as `sim/la2a` and the others). Check it against textbook closed-box results (system resonance fc and Qtc from Vb, Vas, Qts) and against the impedance curve implied by a real 12" guitar speaker's published T/S data.
- **Radiation and mic:** analytic references.
  - Rigid baffled piston: the on-axis near-field pressure has a closed form (the classic sin(k(√(r²+a²) − r)/2) behaviour); the far-field directivity is 2·J1(ka·sinθ)/(ka·sinθ).
  - Capsule averaging: the same Bessel form over the diaphragm.
- **Cone and baffle:** an offline axisymmetric FDTD reference (alternative E) for the modal cone plus Rayleigh integral. Compare mic-position sweeps.
- **Real cabs:** public guitar cab IR sets with documented mic positions (cap, cap edge, cone, distances). Compare **trends**, not exact curves:
  - how the spectrum tilts as the mic moves from cap to edge;
  - the breakup peak frequencies for a given speaker model;
  - proximity bass versus distance;
  - the open-back bass loss.
- **The published frequency response** of a common 12" guitar speaker, with its published T/S parameters dialled in.

## 6. Build order

1. **Driver and box netlist:** closed box, T/S knobs, ngspice deck and comparison, impedance and on-axis SPL curves. (The engine already handles gyrators as ideal transformers or controlled sources; mechanical and acoustic parts are ordinary R/L/C.)
2. **IR engine:** transfer → IR, partitioned convolution, background rebuild, crossfade. Plumbing, tested with the rigid piston.
3. **Rigid piston → mic:** Rayleigh integral in the near field; mic distance, position and angle; capsule averaging and proximity. Check against the analytic piston results.
4. **Cone breakup:** a modal conical shell with material knobs. Check against FDTD, then against real cab IR trends. *This is where the open question gets answered.*
5. **Cabinet:** interior modes (closed) and baffle diffraction plus rear wave (open).
6. **Motor nonlinearity:** Bl(x), Cms(x), voice-coil heating.
7. **Circuit Bench entry**, then a product UI: a cab drawing with a draggable mic, and a "cone view" showing the modes lighting up.
8. **Pass 2: damage** (section 4).

## 7. Open questions and risks

- **Cone realism:** can analytic conical-shell modes plus the Rayleigh integral give convincing breakup, or does it take a small finite-element eigen-solve? Real IR trends and the FDTD reference decide.
- **Paper nonlinearity at breakup:** part of the "speaker breakup" guitarists talk about is nonlinear behaviour of the cone itself, and the hybrid treats the cone as linear. If it's audible at guitar levels, one option is a small nonlinear term on the dominant modes: those few modes would run per sample (a slice of C), with the rest staying in the IR.
- **Rebuild latency versus automation:** is a ~20 ms crossfade acceptable for mic moves? If not, D3 (modal weights).
- **Knob design:** raw T/S parameters are opaque to guitarists. The product probably exposes physical knobs (magnet, cone paper, surround, box size) mapped to T/S, with the raw values in the Bench.
