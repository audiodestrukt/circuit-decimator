// Iron -- a tube line stage into a single-ended, gapped output transformer
// (core/circuit/circuits/SEOutput.h, verified against sim/tubeout/se_out.cir).
// Knobs describe the iron, not DSP: how hard it's driven, the air gap, the
// core size and the steel. Plain C++ so tools can use the same mapping.
#pragma once

#include "circuit/Knobs.h"
#include "circuit/circuits/SEOutput.h"

namespace iron {

enum KnobIndex { kDrive, kGap, kCore, kSteel, kOutput, kMix, kNumKnobs };

inline constexpr cd::Knob kKnobs[kNumKnobs] = {
    { "drive", "Drive", -12, 24, -12, 0, "dB", 1, true },
    { "gap", "Gap", 0.03, 0.3, 0.1, 0.1, "mm", 2, true },
    { "core", "Core", 50, 200, 100, 100, "%", 0, true },
    { "steel", "Steel", 0, 100, 0, 30, "%", 0, true },      // 0 premium .. 100 scrap
    { "output", "Output", -24, 12, -24, 0, "dB", 1, false },
    { "mix", "Mix", 0, 100, 0, 100, "%", 0, false },
};

// Full-scale input -> volts at the grid source at 0 dB drive, and circuit
// volts -> full scale at the output (the stage has ~12.5 dB of gain).
inline constexpr double kGridVolts = 1.0;
inline constexpr double kOutputScale = 1.0 / 4.2;

// Knobs that live on the core device (change without rebuilding matrices).
struct CoreSettings {
    double lg, ac, k, c;
};

inline CoreSettings coreFor(const double* v)
{
    const cd::SEOutputParams d;
    const double steel = std::clamp(v[kSteel] / 100.0, 0.0, 1.0);
    return {
        v[kGap] * 1e-3,                              // mm -> m
        d.ac * v[kCore] / 100.0,                     // cross-section scales with core size
        8.0 * std::pow(80.0 / 8.0, steel),           // pinning: premium 8 A/m .. scrap 80 A/m
        0.5 + (0.1 - 0.5) * steel,                   // reversibility: premium 0.5 .. scrap 0.1
    };
}

// Drive in, compensated out: Drive changes colour, not loudness.
inline double inputGain(const double* v) { return kGridVolts * std::pow(10.0, v[kDrive] / 20.0); }
inline double outputGain(const double* v)
{
    return kOutputScale * std::pow(10.0, (v[kOutput] - v[kDrive]) / 20.0);
}

} // namespace iron
