// ConeEngine -- Cone's audio path: per channel, the speaker cab (driver + box
// circuit per sample, convolved with the physics IR a worker rebuilds when a knob
// moves) at the host rate, a dry path aligned to the convolver's latency for Mix,
// and an auto level that follows the cab's broadband level so moving the mic
// changes the tone rather than the volume.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "ConeParams.h"

#include <memory>

namespace cone {

class Engine {
public:
    void prepare(double sampleRate, int maxBlockSize, const double* knobs);
    void reset();
    int latencySamples() const { return latency; }
    void process(juce::AudioBuffer<float>&, int numInputs, int numOutputs, const double* knobs);
    void release();

    // for the editor's cross-section (UI thread): channel 0's cab, or nullptr before prepare
    const cd::acoustic::Cab* cab() const { return cabs[0].get(); }

private:
    std::array<std::unique_ptr<cd::acoustic::Cab>, 2> cabs;
    double fs = 48000, refBandDb = 0;
    int maxBlock = 512, latency = 64;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> dry { 8192 };
    juce::AudioBuffer<float> dryWork;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> gain;
    juce::SmoothedValue<float> mix;

    void setKnobs(const double* knobs);
    float targetGain(const double* knobs) const;
};

} // namespace cone
