// Cone -- a guitar speaker and cabinet, physically modelled (the cab simulator in
// core/acoustic/CabModel.h; docs/briefs/2026-09-26-speaker-cab.md). The knobs are
// the physical speaker, box and mic; everything else stays at the calibrated
// 12" speaker's values (sim/speaker/calibrate.py). Plain C++ so tools can share it.
#pragma once

#include "acoustic/CabModel.h"
#include "circuit/Knobs.h"

namespace cone {

enum KnobIndex {
    kSize, kPaper, kWeight, kDepth, kDustCap, kDamping,       // speaker
    kVolume, kOpening,                                        // cabinet
    kMicPos, kMicDist, kMicAngle, kMicType,                   // mic
    kAutoLevel, kLevel, kMix,                                 // output
    kNumKnobs
};

inline constexpr cd::Knob kKnobs[kNumKnobs] = {
    { "size", "Size", 6, 15, 10.5, 12, "in", 1, true },
    { "paper", "Paper Stiffness", 1, 8, 4, 4.8, "GPa", 1, true },
    { "weight", "Paper Weight", 250, 900, 450, 441, "kg/m3", 0, true },
    { "depth", "Cone Depth", 3, 8, 5, 6.0, "cm", 1, true },
    { "dustcap", "Dust Cap", 0.1, 3, 0.8, 0.7, "g", 2, true },
    { "damping", "Paper Damping", 0.01, 0.2, 0.05, 0.031, "", 3, true },
    { "volume", "Box Volume", 10, 200, 50, 50, "l", 0, true },
    { "opening", "Back Opening", 0, 2000, 200, 0, "cm2", 0, true },
    { "micpos", "Mic Position", 0, 15, 0, 0, "cm", 1, true },
    { "micdist", "Mic Distance", 0.5, 60, 5, 2.5, "cm", 1, true },
    { "micangle", "Mic Angle", 0, 60, 0, 0, "deg", 0, true },
    { "mictype", "Mic", 0, 2, 0, 0, "", 0, false },          // 0 dynamic (measured), 1 cardioid, 2 omni
    { "autolevel", "Auto Level", 0, 1, 0, 1, "", 0, false },
    { "level", "Level", -24, 24, -24, 0, "dB", 1, false },
    { "mix", "Mix", 0, 100, 0, 100, "%", 0, false },
};

inline constexpr const char* kMicNames[] = { "Dynamic (measured)", "Ideal cardioid", "Ideal omni" };

// Digital full scale = this many volts at the speaker's terminals (~25 W peak into 8 ohm).
inline constexpr double kAmpVolts = 20.0;

// Knobs -> the cab model's parameters (SI units), handed to `set(index, value)`.
template <typename Set>
inline void applyKnobs(const double* v, Set&& set)
{
    namespace a = cd::acoustic;
    set(a::kConeRadius, v[kSize] * 0.01058);       // nominal inches -> radiating radius
    set(a::kYoungs, v[kPaper] * 1e9);
    set(a::kDensity, v[kWeight]);
    set(a::kDepth, v[kDepth] * 1e-2);
    set(a::kCapMass, v[kDustCap] * 1e-3);
    set(a::kLoss, v[kDamping]);
    set(a::kVb, v[kVolume] * 1e-3);
    set(a::kOpenArea, v[kOpening] * 1e-4);
    set(a::kMicOffset, v[kMicPos] * 1e-2);
    set(a::kMicDistance, v[kMicDist] * 1e-2);
    set(a::kMicAngle, v[kMicAngle] * M_PI / 180);
    const int mic = (int) std::lround(v[kMicType]);
    set(a::kMicModel, mic == 0 ? 1.0 : 0.0);
    set(a::kMicPattern, mic == 2 ? 1.0 : 0.5);
    set(a::kMicFace, mic == 0 ? a::mics::kDynamicFace : 0.0);   // the measured mic's body reflects
}

} // namespace cone
