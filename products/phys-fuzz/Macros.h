// Phys Fuzz -- the focused fuzz. A few player-facing knobs, each mapped onto
// the workbench's full component surface (core/circuit/Knobs.h). Plain C++ so
// the search tools can use the same mapping (tools/ff_capi.cpp).
//
// The curves are a first pass: tune them by ear in Circuit Bench / the plugin,
// and use the search map to see where they travel.
#pragma once

#include "circuit/Knobs.h"

namespace pf {

enum KnobIndex { kFuzz, kVolume, kBattery, kAge, kTemperature, kOutput, kNumKnobs };

inline constexpr cd::Knob kKnobs[kNumKnobs] = {
    { "fuzz", "Fuzz", 0, 1, 0, 1, "", 2, true },
    { "volume", "Volume", 0, 1, 0, 0.5, "", 2, false },
    { "battery", "Battery", 0, 100, 0, 100, "%", 0, true },        // charge left
    { "age", "Age", 0, 100, 0, 0, "%", 0, true },                  // mint .. barn find
    { "temperature", "Temperature", -20, 80, -20, 27, "C", 0, true },
    { "output", "Output", -24, 24, -24, 0, "dB", 1, false },
};

// Phys Fuzz knob values -> workbench knob values (cd::KnobIndex order).
inline void toWorkbench(const double* v, double* wb)
{
    for (int i = 0; i < cd::kNumKnobs; ++i) wb[i] = cd::kKnobs[i].def;
    wb[cd::kFuzz] = v[kFuzz];
    wb[cd::kVolume] = v[kVolume];
    wb[cd::kOutput] = v[kOutput];
    wb[cd::kTemperature] = v[kTemperature];

    // Battery: EMF falls and internal resistance climbs as it drains; a modest
    // supply cap lets the sag pump with the playing instead of just dropping.
    const double drain = 1.0 - std::clamp(v[kBattery] / 100.0, 0.0, 1.0);
    // 0% is tuned to the edge of death (still sputters), not silence: below
    // ~4.4 V with this sag the circuit just stops, which is no use on a knob.
    wb[cd::kBattery] = 9.0 - 4.6 * std::pow(drain, 1.5);       // 9 V .. 4.4 V
    wb[cd::kBatteryRes] = std::exp(drain * std::log(1150.0));   // 1 Ohm .. 1.15 kOhm
    wb[cd::kSupplyCap] = 4.7;

    // Age: junctions leak, electrolytics dry out, gain fades, bias drifts cold.
    const double age = std::clamp(v[kAge] / 100.0, 0.0, 1.0);
    wb[cd::kJunctionLeak] = 0.75 * age;
    wb[cd::kCapLeak] = 0.55 * std::pow(age, 1.5);
    wb[cd::kBypassCap] = 20.0 * std::pow(1.5 / 20.0, age);      // 20 uF .. 1.5 uF
    wb[cd::kQ1Gain] = 250.0 * std::pow(60.0 / 250.0, age);
    wb[cd::kQ2Gain] = 250.0 * std::pow(120.0 / 250.0, age);
    wb[cd::kBias] = 5.6 * std::pow(7.5 / 5.6, age);             // drifts toward cold
}

struct Preset {
    const char* name;
    double battery, age, temperature, fuzz;
};

// Volume and Output stay where the player left them.
inline constexpr Preset kPresets[] = {
    { "Fresh", 100, 0, 27, 1 },
    { "Tired Battery", 50, 0, 27, 1 },
    { "Last Gasp", 3, 0, 27, 1 },
    { "Old Stock", 100, 45, 27, 1 },
    { "Barn Find", 60, 85, 27, 1 },
    { "Heatwave", 100, 30, 65, 1 },
    { "Cold Garage", 100, 10, -15, 1 },
    { "Half Fuzz, Half Dead", 35, 40, 27, 0.4 },
};

} // namespace pf
