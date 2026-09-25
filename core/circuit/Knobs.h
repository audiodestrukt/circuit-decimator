// Knobs.h -- the plugin's controls: ranges, curves, defaults, and how they map
// onto circuit values. Shared by the JUCE processor, the C API used by the
// search (search/explore.py) and anything else that turns knob positions into
// a FuzzFaceParams, so they can't drift apart. No JUCE dependency.
#pragma once

#include "FuzzFace.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace cd {

struct Knob {
    const char* id;
    const char* name;
    double min, max;
    double centre;     // value at mid-travel (log-ish curve); <= min means linear
    double def;
    const char* unit;
    int decimals;
    bool searchable;   // part of the "destroy" space the explorer searches
};

enum KnobIndex {
    kFuzz, kVolume, kInput, kOutput,
    kBattery, kBatteryRes, kSupplyCap, kBias, kQ1Gain, kQ2Gain,
    kJunctionLeak, kCapLeak, kBypassCap, kTemperature,
    kNumKnobs
};

inline constexpr Knob kKnobs[kNumKnobs] = {
    // player knobs
    { "fuzz", "Fuzz", 0, 1, 0, 1, "", 2, true },
    { "volume", "Volume", 0, 1, 0, 0.5, "", 2, false },
    { "input", "Input Level", -24, 24, -24, 0, "dB", 1, false },
    { "output", "Output", -36, 30, -36, 0, "dB", 1, false },  // wide: starved circuits get very quiet
    // destroy knobs
    { "battery", "Battery", 0.5, 12, 0.5, 9, "V", 2, true },
    { "batteryRes", "Battery Sag", 1, 5000, 200, 1, "Ohm", 0, true },
    { "supplyCap", "Supply Cap", 0.1, 470, 10, 0.1, "uF", 2, true },
    { "bias", "Bias Trim", 1, 15, 5.6, 5.6, "kOhm", 2, true },
    { "q1Gain", "Q1 hFE", 5, 600, 100, 250, "", 0, true },
    { "q2Gain", "Q2 hFE", 5, 600, 100, 250, "", 0, true },
    { "junctionLeak", "Junction Leak", 0, 1, 0, 0, "", 2, true },
    { "capLeak", "Input Cap Leak", 0, 1, 0, 0, "", 2, true },
    { "bypassCap", "Bypass Cap", 0.1, 47, 5, 20, "uF", 2, true },
    { "temperature", "Temperature", -40, 120, -40, 27, "C", 0, true },
};

// Pickup EMF in volts for a full-scale input sample, and the output scale that
// puts the healthy circuit's peak near -1 dBFS (both match sim/render.py).
inline constexpr double kPickupVolts = 0.15;
inline constexpr double kOutputScale = 0.26;

// Same curve as juce::NormalisableRange::setSkewForCentre (non-symmetric).
inline double knobSkew(const Knob& k)
{
    return k.centre > k.min ? std::log(0.5) / std::log((k.centre - k.min) / (k.max - k.min)) : 1.0;
}

inline double knobFromNormalised(const Knob& k, double p)
{
    p = std::clamp(p, 0.0, 1.0);
    if (k.centre > k.min && p > 0.0) p = std::exp(std::log(p) / knobSkew(k));
    return k.min + (k.max - k.min) * p;
}

inline double knobToNormalised(const Knob& k, double v)
{
    const double p = std::clamp((v - k.min) / (k.max - k.min), 0.0, 1.0);
    return k.centre > k.min ? std::pow(p, knobSkew(k)) : p;
}

// "Leak" knobs: 0 = healthy (1 TOhm), then log-sweep from rHi down to rLo.
inline double leakToOhms(double amount, double rHi, double rLo)
{
    if (amount <= 0.001) return 1e12;
    return std::exp(std::log(rHi) + amount * (std::log(rLo) - std::log(rHi)));
}

// Knob values (in their own units, indexed by KnobIndex) -> circuit values.
// Input/output levels are gains around the circuit, not part of it.
inline FuzzFaceParams knobsToCircuit(const double* v)
{
    FuzzFaceParams p;
    p.fuzz = v[kFuzz];
    p.vol = v[kVolume];
    p.vcc = v[kBattery];
    p.rbat = v[kBatteryRes];
    p.cbulk = v[kSupplyCap] * 1e-6;
    p.rc2b = v[kBias] * 1e3;
    p.bf1 = v[kQ1Gain];
    p.bf2 = v[kQ2Gain];
    p.rleak1 = p.rleak2 = leakToOhms(v[kJunctionLeak], 1e8, 3e4);
    p.rleakCin = leakToOhms(v[kCapLeak], 5e6, 1e4);
    p.cfz = v[kBypassCap] * 1e-6;
    p.tempC = v[kTemperature];
    return p;
}

struct Preset {
    const char* name;
    std::vector<std::pair<const char*, double>> values; // unlisted knobs -> default
};

// Hand-made "destroy" presets; mirror PRESETS in sim/render.py. Leak amounts
// are the knob positions that give the same resistance (leakToOhms inverted).
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> ps {
        { "Healthy", {} },
        { "Fuzz Low", { { "fuzz", 0.15 } } },
        { "Starved 6V", { { "battery", 6 } } },
        { "Starved 4.5V", { { "battery", 4.5 } } },
        { "Edge of Death 3.6V", { { "battery", 3.6 } } },
        { "Dying Battery", { { "battery", 7.5 }, { "batteryRes", 3000 }, { "supplyCap", 4.7 } } },
        { "Bias Cold", { { "bias", 8.2 } } },
        { "Bias Hot", { { "bias", 3.3 } } },
        { "Weak Q1", { { "q1Gain", 40 } } },
        { "Leaky Junctions", { { "junctionLeak", 0.716 } } },
        { "Leaky Input Cap", { { "capLeak", 0.63 } } },
        { "Dried Bypass Cap", { { "bypassCap", 1 } } },
        { "Hot Day", { { "temperature", 70 } } },
        { "Frozen", { { "temperature", -20 } } },
        { "Wrecked", { { "battery", 4 }, { "batteryRes", 800 }, { "supplyCap", 22 }, { "q1Gain", 40 },
                       { "junctionLeak", 0.8 }, { "temperature", 55 }, { "bypassCap", 2 } } },
    };
    return ps;
}

// Knob values for a preset: defaults, overridden by the preset's entries.
inline void presetKnobs(const Preset& preset, double* v)
{
    for (int i = 0; i < kNumKnobs; ++i) {
        v[i] = kKnobs[i].def;
        for (auto& [id, val] : preset.values)
            if (std::string_view(kKnobs[i].id) == id) v[i] = val;
    }
}

} // namespace cd
