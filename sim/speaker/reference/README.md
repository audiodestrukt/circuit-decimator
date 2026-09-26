# Reference data for calibrating the cab model

## Eminence Legend 1258 (12", 8 ohm, paper cone, paper edge, paper dust cap)

- `legend1258.pdf`: the manufacturer's spec sheet
  (https://www.parts-express.com/pedocs/specs/290-486--eminence-legend-1258-spec-sheet.pdf).
  Measurement conditions (page 2): 2.83 V at 1 m, the speaker flush in a
  2 ft x 2 ft baffle built into the wall of a 2700 cu ft anechoic chamber
  (effectively an infinite baffle, no box). The chart is 1/6-octave smoothed.
- `legend1258_spl.csv`, `legend1258_impedance.csv`: the chart's two curves,
  extracted from the PDF's vector paths, not by tracing an image. The axes are
  calibrated from the grid lines, which land exactly on 20, 30 ... 20 000 Hz;
  SPL 70–110 dB linear, impedance 7–60 ohm log.
- Thiele-Small parameters, from https://eminence.com/products/legend_1258:
  Fs 94 Hz, Re 7.44 ohm, Le 0.7 mH, Qms 6.15, Qes 1.18, Qts 0.99, Vas 32.5 l,
  Sd 506.7 cm^2, Xmax 0.48 mm, Mms 32 g (with air load), Bl 10.9 T m,
  1.5" (38 mm) voice coil, sensitivity 100.1 dB.
  Derived: Cms = 8.96e-5 m/N, Rms = 3.07 N s/m (Qes recomputes to 1.18 and
  Vas to 32.6 l: the set is consistent).

The data is the manufacturer's. It's kept here only as a calibration reference.
