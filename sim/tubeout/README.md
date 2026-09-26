# Single-ended output transformer

Line source -> 12AU7 (Koren) -> 4:1 gapped-steel output transformer -> 10k
load. The plate current flows through the primary, so the core sits on a DC
bias: asymmetric B-H excursions, even harmonics, earlier low-frequency
saturation. The air gap sets how much bias the core sees. Values are plausible
generic, not calibrated to a product.

```
se_out.cir    the spec (ngspice): B+ ramps up from 0 (uic) so the core
              magnetises itself, then the tone plays once settled
compare.py    bias, gain + THD (even/odd) vs level x frequency, ngspice vs the
              engine (build/circuit_render seout);
              --power-on  how the power-on ramp changes the core's resting state
              --listen    riff/bass at 2 V: real core, ideal core, 0.05/0.2 mm gap
```

Engine netlist: `core/circuit/circuits/SEOutput.h`.

Results (defaults: 4000 turns, 6 cm^2, 0.1 mm gap, silicon steel):

- Bias: plate 242.27 V, 9.7 mA; core rests at -0.41 T. Engine and ngspice
  agree on bias (0.003 T), gain (0.01 dB) and THD (identical to 3 decimals at
  most points; the quietest level differs by ~0.02%, where the small
  power-on-history difference in resting magnetization matters most).
- Character: flat to 30 Hz (-0.9 dB); even harmonics dominate as drive rises
  (4.7% even vs 1.3% odd at 6 V, 30 Hz); distortion climbs toward the bass.
- Gap: 0.2 mm -> less bias (-0.22 T), leaner bass, more even; 0.05 mm -> much
  more bias (-0.71 T), fuller bass, more odd grit.
- Audible: vs an ideal core of the same small-signal inductance (70 H), the
  iron adds -32 dB on bass and -47.5 dB on guitar.

Power-on history matters (hysteresis remembers it): a fast B+ ramp overshoots
the plate current (70 mA peak for a 20 ms ramp vs 9.7 mA settled) and leaves the
core with more remanent flux (-0.37 T) than a gentle one (-0.28 T at 1 s). The
engine's initDC is the gentle limit (initial magnetization curve); compare.py
uses a 3 s ramp so ngspice lands near it. A realistic "cold start" could be a
feature later.
