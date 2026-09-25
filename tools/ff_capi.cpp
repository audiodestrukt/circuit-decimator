// libfuzzface -- C API over core/circuit/ for Python (ctypes). Used by search/.
//
// Knob values are in plugin units (core/circuit/Knobs.h); rendering takes input already
// at the oversampled rate, applies the same input/output gains as the plugin
// (minus oversampling filters and DC blocking, which the caller does).
#include "circuit/Knobs.h"
#include "phys-fuzz/Macros.h"

extern "C" {

int ff_num_knobs() { return cd::kNumKnobs; }
const char* ff_knob_id(int i) { return cd::kKnobs[i].id; }
const char* ff_knob_unit(int i) { return cd::kKnobs[i].unit; }
double ff_knob_min(int i) { return cd::kKnobs[i].min; }
double ff_knob_max(int i) { return cd::kKnobs[i].max; }
double ff_knob_default(int i) { return cd::kKnobs[i].def; }
int ff_knob_searchable(int i) { return cd::kKnobs[i].searchable ? 1 : 0; }
double ff_knob_from_normalised(int i, double p) { return cd::knobFromNormalised(cd::kKnobs[i], p); }
double ff_knob_to_normalised(int i, double v) { return cd::knobToNormalised(cd::kKnobs[i], v); }

int ff_num_presets() { return (int) cd::presets().size(); }
const char* ff_preset_name(int i) { return cd::presets()[(size_t) i].name; }
void ff_preset_knobs(int i, double* out) { cd::presetKnobs(cd::presets()[(size_t) i], out); }

// Phys Fuzz macro knobs -> workbench knobs (products/phys-fuzz/Macros.h)
int ff_physfuzz_num_knobs() { return pf::kNumKnobs; }
const char* ff_physfuzz_knob_id(int i) { return pf::kKnobs[i].id; }
double ff_physfuzz_knob_default(int i) { return pf::kKnobs[i].def; }
void ff_physfuzz_to_workbench(const double* phys, double* wb) { pf::toWorkbench(phys, wb); }

// knobs: ff_num_knobs() values. in/out: n samples at fs (oversampled rate),
// in digital full scale. stats (may be null): [newton avg, newton max, failures].
void ff_render(const double* knobs, const float* in, float* out, int n, double fs, int maxIterations,
               double* stats)
{
    cd::FuzzFace ff;
    ff.maxIterations = maxIterations;
    ff.setParams(cd::knobsToCircuit(knobs));
    ff.prepare(fs);
    const double gIn = std::pow(10.0, knobs[cd::kInput] / 20.0) * cd::kPickupVolts;
    const double gOut = std::pow(10.0, knobs[cd::kOutput] / 20.0) * cd::kOutputScale;
    long sum = 0;
    int mx = 0;
    for (int i = 0; i < n; ++i) {
        out[i] = (float) (ff.process(in[i] * gIn) * gOut);
        sum += ff.lastIterations;
        mx = std::max(mx, ff.lastIterations);
    }
    if (stats) {
        stats[0] = n > 0 ? (double) sum / n : 0;
        stats[1] = mx;
        stats[2] = (double) ff.failures;
    }
}

}
