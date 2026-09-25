// Embedded entry point for the fuzz core: the single-precision DK solver behind
// a tiny C interface that a Daisy (libDaisy) or Teensy (Audio library) wrapper
// calls from its audio callback. Also a compile/size/codegen check for
// Cortex-M7: `make -C tools/embedded` (needs arm-none-eabi-g++).
#include "circuit/FuzzFaceDK.h"

namespace {
cd::FuzzFaceDKf circuit;
constexpr float kPickupVolts = 0.15f;   // full-scale input -> pickup EMF (core/circuit/Knobs.h)
constexpr float kOutputScale = 0.26f;   // circuit volts -> full scale
}

extern "C" {

// sampleRate = the rate process() is called at (i.e. after any oversampling)
void cd_fuzz_init(float sampleRate)
{
    circuit.maxIterations = 6;
    circuit.prepare(sampleRate);
}

// Circuit values (not knob positions): call at control rate, it rebuilds the
// solver matrices (~11x11 LU + 12 solves in double).
void cd_fuzz_set(const cd::FuzzFaceParams* params)
{
    circuit.setParams(*params);
}

// in/out: full-scale floats, DC-coupled output (high-pass it after decimation)
void cd_fuzz_process(const float* in, float* out, int n)
{
    for (int i = 0; i < n; ++i) out[i] = circuit.process(in[i] * kPickupVolts) * kOutputScale;
}

}

#ifdef CD_FUZZ_TEST_MAIN
// link check: a fake audio loop so nothing is optimised away
volatile float sink;
int main()
{
    cd_fuzz_init(96000.0f);
    float in[48], out[48];
    for (int block = 0; block < 1000; ++block) {
        for (int i = 0; i < 48; ++i) in[i] = (float) ((block * 48 + i) % 437) / 437.0f - 0.5f;
        cd_fuzz_process(in, out, 48);
        sink = out[0];
    }
}
#endif
