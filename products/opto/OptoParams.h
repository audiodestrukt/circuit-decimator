// Opto -- an LA-2A-style optical leveling amplifier: the whole unit as a
// netlist (core/circuit/circuits/LA2A.h, verified against sim/la2a/la2a.cir).
// The front panel is the original's: Peak Reduction (how hard the T4 is
// driven) and Gain (makeup), plus Mix. Plain C++ so tools can share it.
#pragma once

#include "circuit/Knobs.h"

#include <algorithm>
#include <cmath>

namespace opto {

enum KnobIndex { kPeak, kGain, kMix, kNumKnobs };

inline constexpr cd::Knob kKnobs[kNumKnobs] = {
    { "peak", "Peak Reduction", 0, 100, 0, 60, "", 0, true },
    { "gain", "Gain", -20, 10, -20, 0, "dB", 1, true },
    { "mix", "Mix", 0, 100, 0, 100, "%", 0, false },
};

// Level calibration: digital full scale = +4 dBu peak at the input
// transformer. The amplifier has 20 log10(gain wiper) + 40.2 dB of gain with
// the cell dark; Gain 0 dB puts the wiper where that is 30.2 dB, and the output
// is scaled so that is unity.
inline constexpr double kInVolts = 1.7367;                 // +4 dBu peak
inline constexpr double kUnityGainDb = 30.2;
inline double outputScale() { return 1.0 / (kInVolts * std::pow(10.0, kUnityGainDb / 20)); }

// Gain knob (dB) -> Gain pot wiper (0..1): +10 dB is the pot fully up.
inline double gainWiper(double db) { return std::clamp(std::pow(10.0, (db - 10.0) / 20.0), 1e-4, 1.0); }

// Peak Reduction knob (0..100) -> pot wiper, audio taper: 40 dB of travel,
// so the threshold moves smoothly across the whole knob.
inline double peakWiper(double k) { return std::clamp(std::pow(10.0, 2.0 * (k / 100.0 - 1.0)), 1e-4, 1.0); }

} // namespace opto
