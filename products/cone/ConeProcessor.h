#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "ConeEngine.h"
#include "engine/Params.h"

class ConeProcessor : public juce::AudioProcessor {
public:
    ConeProcessor();

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override { engine.release(); }
    void reset() override { engine.reset(); }
    bool isBusesLayoutSupported(const BusesLayout&) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Cone"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.1; }

    int getNumPrograms() override;
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram(int) override;
    const juce::String getProgramName(int) override;
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    juce::AudioProcessorValueTreeState apvts;
    cone::Engine engine;

private:
    void knobs(double* out) const;
    int currentProgram = 0;
};
