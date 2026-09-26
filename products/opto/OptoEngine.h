// OptoEngine -- Opto's audio path: per channel, the LA-2A netlist (audio path
// + sidechain) at 2x oversampling with the sidechain at the host rate, pots
// smoothed at control rate, a dry path aligned to the oversampler's latency
// for Mix, and telemetry for the editor (gain reduction, output level, the
// T4's panel light and cell resistance).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "OptoParams.h"
#include "circuit/circuits/LA2A.h"

namespace opto {

class Engine {
public:
    void prepare(double sampleRate, int maxBlockSize, const double* knobs);
    void reset();
    int latencySamples() const { return latency; }
    void process(juce::AudioBuffer<float>&, int numInputs, int numOutputs, const double* knobs);

    // ---- telemetry, written by the audio thread ----
    std::atomic<float> gainReduction { 0 };   // dB, the more-compressed channel
    std::atomic<float> outputRms { 0 };       // linear, full scale = 1
    std::atomic<float> panelLight { 0 };      // T4 EL panel light, 0..~1
    std::atomic<float> cellOhms { 5e6f };
    std::atomic<long> failures { 0 };

private:
    static constexpr int osLog2 = 1;          // 2x
    static constexpr int controlInterval = 32;
    static constexpr int sidechainEvery = 2;  // sidechain at the host rate

    std::array<cd::LA2A, 2> ch;
    double osRate = 96000;
    int maxBlock = 512;
    int latency = 0;

    juce::dsp::Oversampling<float> oversampling { 2, osLog2,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dry { 8192 };
    juce::AudioBuffer<float> work, dryWork;
    std::array<juce::dsp::IIR::Filter<float>, 2> dcBlock;
    juce::SmoothedValue<double> gainDb, peak;
    juce::SmoothedValue<float> mix;
    float rmsState = 0;

    void setTargets(const double* knobs, bool snap);
    void applyPots();
};

} // namespace opto
