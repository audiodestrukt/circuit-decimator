# Shin-Ei Companion FY-2 "Fuzzmaster"

Two collector-feedback silicon stages (2SC536F-class NPN, hFE 160-320) with
the Fuzz pot panning between their collectors, a passive mid scoop, and a
50k volume pot. Traced from `products/phys-fuzz/reference/shin-ei_fy2_fuzzmaster.pdf`
(S. Castledine, 2002); the same pickup, battery and transistor model as the
Fuzz Face deck so the two share one knob surface.

```
shinei.cir    the spec (ngspice); destroy knobs as .params like fuzzface.cir
compare.py    ngspice vs the general engine (build/circuit_render shinei) on the
              synthetic riff, per preset: bias, waveform error, solver stats,
              renders/<preset>_{spice,engine}.wav, renders/compare.png
```

The realtime version is a netlist for the general engine,
`core/circuit/circuits/ShinEi.h`, not a hand-built solver. `knobsToShinEi()`
there maps the shared knobs (Knobs.h) onto it: Bias Trim scales Q2's 1M2
feedback resistor, Junction Leak spans 1 GOhm..300 kOhm (across megohm bias
resistors, the Fuzz Face's 30 kOhm floor would just saturate the stage),
Bypass Cap has no counterpart.

Results (riff, 4x 48 kHz, every preset):

- Bias identical to ngspice to the printed precision: Q1 collector 3.595 V,
  Q2 collector 1.021 V (Q2 sits low on its 100k-starved supply; that is the
  gate).
- v(out) matches ngspice to 2.5-3.6% RMS (the Fuzz Face's own solver is at
  6-12% on its deck: the residual is ngspice's variable step on the edges),
  Newton 1.3 iterations/sample average, no failures, ~20x realtime.
- Output: 110 mV peak from a 150 mV pickup, ~30 dB below the Fuzz Face, so the
  plugin scales it by `kOutputScaleShinEi` = 8.1 (compare.py prints it).

The Fuzz control is a pan, not a gain: `fuzz=1` puts the wiper at the Q2 end
(both stages mixed, then the whole 50k to the scoop), `fuzz=0` wires Q1's
collector straight in with Q2 behind the pot. It changes texture more than
amount, as on the pedal.
