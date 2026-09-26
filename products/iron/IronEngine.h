// IronEngine -- Iron's audio path: per channel, the single-ended output stage
// netlist (SEOutput) on the general engine at 4x oversampling, with smoothed
// core settings, a dry path aligned to the oversampler's latency for Mix, the
// cold-start ramp, and telemetry for the editor (B-H trace, node voltages).
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "IronParams.h"
#include "ui/BHLoopView.h"

namespace iron {

class Engine {
public:
    // telemetry starts at the knob defaults: an editor can open before prepare()
    Engine()
    {
        for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) kKnobs[i].def;
    }

    void prepare(double sampleRate, int maxBlockSize, const double* knobs);
    void reset();
    int latencySamples() const { return latency; }
    void process(juce::AudioBuffer<float>&, int numInputs, int numOutputs, const double* knobs);

    // Power the stage off and back on with a fast supply ramp (audio thread picks it up).
    void coldStart() { coldRequested = true; }

    // ---- telemetry (channel 0), written by the audio thread ----
    cd::ui::BHTrace trace;                              // recent B-H points
    enum Probe { Grid, Cathode, Plate, Supply, Primary, Out, kProbes };
    std::array<std::atomic<float>, kProbes> probeMean {}, probeSwing {};
    std::atomic<float> fluxMean { 0 }, supplyVolts { 0 };
    std::array<std::atomic<float>, kNumKnobs> knobsInUse {};
    std::atomic<float> newtonAverage { 0 };
    std::atomic<long> failures { 0 };

private:
    static constexpr int osLog2 = 2;          // 4x
    static constexpr int controlInterval = 32;
    static constexpr double coldRampSeconds = 0.03;

    struct Channel {
        cd::SEOutput stage;
        int probeNodes[kProbes] {};
    };
    std::array<Channel, 2> ch;
    int numChannels = 2;
    double osRate = 192000;
    int maxBlock = 512;
    int latency = 0;
    std::atomic<bool> coldRequested { false };
    long coldSamples = -1;                    // >= 0 while ramping after a cold start
    double bplus = 250;

    juce::dsp::Oversampling<float> oversampling { 2, osLog2,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple, true, true };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dry { 8192 };
    juce::AudioBuffer<float> work, dryWork;
    std::array<juce::dsp::IIR::Filter<float>, 2> dcBlock;

    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Multiplicative> lg, ac, kpin;
    juce::SmoothedValue<double> crev;
    juce::SmoothedValue<float> gIn, gOut, mix;
    int traceDecim = 0;

    void applyCore();
    void setTargets(const double* knobs, bool snap);
};

} // namespace iron
