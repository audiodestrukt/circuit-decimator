// FuzzEngine -- the realtime audio path every product shares: mono sum ->
// pickup -> DK circuit solver at 4x oversampling -> DC block -> output, with
// per-parameter smoothing, solver health stats and node telemetry for the
// circuit view. Products own their parameters and hand the engine a full
// knob vector (core/circuit/Knobs.h order, in knob units) each block.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "circuit/Knobs.h"

namespace cd {

class FuzzEngine {
public:
    FuzzEngine();

    void prepare(double sampleRate, int maxBlockSize, const double* knobs);
    void reset();
    int latencySamples() const;

    // Mono circuit: inputs are summed, the result is copied to every output.
    void process(juce::AudioBuffer<float>&, int numInputs, int numOutputs, const double* knobs);

    // "Power cycle": re-solve the DC operating point on the next block.
    void requestCircuitReset() { resetRequested = true; }

    // ---- telemetry (written by the audio thread, read by UIs) ----
    static constexpr int kNodes = FuzzFace::N;
    std::array<std::atomic<float>, kNodes> nodeMean {}, nodeMin {}, nodeMax {};
    std::array<std::atomic<float>, kNumKnobs> knobsInUse {};  // what the circuit is set to
    std::atomic<float> newtonAverage { 0 };
    std::atomic<int> newtonMax { 0 };
    std::atomic<long> solverFailures { 0 };

private:
    void processChunk(juce::AudioBuffer<float>&, int start, int n, int numInputs, int numOutputs);
    void setTargets(const double* knobs);

    static constexpr int osFactorLog2 = 2;     // 4x: fuzz output is square-ish
    static constexpr int controlInterval = 16; // oversampled samples per param update

    int maxBlock = 512;
    std::atomic<bool> resetRequested { false };
    FuzzFaceDK circuit;   // realtime DK solver (FuzzFace.h is its reference)
    FuzzFaceParams target;
    juce::dsp::Oversampling<float> oversampling { 1, osFactorLog2,
        juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, true };
    juce::AudioBuffer<float> mono;
    juce::dsp::IIR::Filter<float> dcBlock;

    // one smoother per solver parameter, stepped at the oversampled rate;
    // resistances/caps/gains that span decades are smoothed in the log domain
    struct Smoothed {
        double FuzzFaceParams::* field;
        bool logDomain;
        juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> value;
        double current() const { return logDomain ? std::exp(value.getCurrentValue()) : value.getCurrentValue(); }
    };
    std::vector<Smoothed> smoothers;
    juce::SmoothedValue<float> inputGain, outputGain;
};

} // namespace cd
