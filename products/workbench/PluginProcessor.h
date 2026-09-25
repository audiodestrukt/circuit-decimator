#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "engine/FuzzEngine.h"
#include "engine/Params.h"

class CircuitDecimatorProcessor : public juce::AudioProcessor {
public:
    CircuitDecimatorProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    void reset() override;
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Circuit Decimator"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    // programs = the "destroy" presets from sim/render.py, in plugin units
    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;

    // Set knobs from values in their own units (core/circuit/Knobs.h order). Without
    // includeLevels, Volume / Input Level / Output keep the player's settings.
    void setKnobs(const double* values, bool includeLevels);

    // the shared audio path (core/engine): telemetry for the circuit view and bench
    cd::FuzzEngine engine;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void currentKnobs(double* out) const;

    int currentProgram = 0;
};
