# Tube mic pre

150 ohm mic -> 1:10 input transformer (Jiles-Atherton core, see ../transformer)
-> 12AX7 common-cathode stage (Koren curve model, grid conduction,
datasheet inter-electrode capacitances) -> output coupling cap. Ideal 250 V
supply; power supplies are a separate module for later.

```
tubepre.cir   the spec (ngspice); ideal transformer as E/F sources + 0 V sense
compare.py    gain + THD vs mic level at 50 Hz / 1 kHz, ngspice vs the engine
              (build/tubepre_render); --listen renders riff/bass at -50/-35/-20
              dBu with the hysteretic core and an ideal core -> renders/
```

The realtime version is a netlist for the general engine,
`core/circuit/circuits/TubePre.h`, not a hand-built solver.

Results:

- Bias: plate 170.1 V, cathode 1.198 V (0.8 mA) in both ngspice and the engine.
- Gain 54 dB (transformer ~19 dB + tube ~35 dB); engine matches ngspice to
  0.01 dB and THD to <1% relative at every level from -60 to -10 dBu.
- Character: second harmonic from the tube rising ~3.2x per 10 dB, turning
  odd (clipping) around -10 dBu at the mic; the iron adds ~-40 dB on bass,
  ~-55 dB on guitar at mic levels (low-level hysteresis region).

Gotcha: ngspice glitches on these hysteresis decks when many copies run at
once: identical runs take different timestep sequences and now and then a
wrong trajectory (4.8% THD where the answer is 0.06%). Its OpenMP threading
makes it much worse (the harnesses set OMP_NUM_THREADS=1), but it still
happens occasionally under concurrency. Run one at a time it is deterministic
and matches the engine, so compare.py and ../transformer/core_sweep.py run
ngspice sequentially (a few seconds) and parallelise only the engine. The
engine itself is deterministic.
