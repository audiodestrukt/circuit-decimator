# Transformer

A 1:10 mic input transformer with a hysteretic core. The ngspice decks are the
spec; `core/circuit/Transformer.h` is the realtime model and matches them.
Values are a plausible generic nickel-core mic transformer, not calibrated to
any product.

```
xfmr_linear.cir   linear model, physical (coupled windings) and referred-to-primary
                  forms in one deck; they agree exactly
xfmr_core.cir     referred form with a Jiles-Atherton core (B-driven, delta_M
                  correction) as behavioural sources + integrators
core_sweep.py     THD vs level x frequency and B-H loops -> core_signature.png;
                  --compare also runs the realtime model (build/xfmr_render)
listen.py         riff + sub bass at -20/0/+10 dBu through core and linear cores,
                  plus the difference ("what the iron adds") -> renders/
```

What it shows:

- Linear: flat 3 Hz to ~90 kHz with the default damping network; undamped, a
  small resonance near 54 kHz; heavier damping pulls the top end into the
  audio band (a tone knob).
- Core: mostly odd-harmonic distortion that rises as frequency falls (flux ~
  V/f) and has the iron "bathtub" shape vs level: hysteresis at low levels,
  a dip, then saturation (~0.56 T) at high levels.
- Realtime vs ngspice: THD within 0.1% median (1.2% worst), level within
  0.006%; ~25-34x realtime at 192 kHz, 2 Newton iterations per sample.
- Audible: on sub-heavy bass the iron adds -41 dB (at -20 dBu) to -30 dB (at
  +10 dBu) of new content between 20 and 200 Hz; on guitar it is -48 to -55 dB.

Gotcha: start the drive smoothly (the decks fade in over 4 cycles). A sine that
starts at its peak, or at a zero crossing, leaves a DC flux offset stuck in the
core's remanence (inrush), which skews loops and distortion numbers.
