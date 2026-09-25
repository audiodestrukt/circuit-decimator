#pragma once

#include "PluginProcessor.h"
#include "ui/CircuitView.h"

// Live schematic on the left, the knobs on the right.
class CircuitDecimatorEditor : public juce::AudioProcessorEditor {
public:
    explicit CircuitDecimatorEditor(CircuitDecimatorProcessor& p)
        : AudioProcessorEditor(p), circuit(p.engine), knobs(p)
    {
        addAndMakeVisible(circuit);
        addAndMakeVisible(knobs);
        setResizable(true, true);
        setResizeLimits(800, 480, 2400, 1400);
        setSize(1200, 660);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1b1c1e)); }

    void resized() override
    {
        auto r = getLocalBounds();
        knobs.setBounds(r.removeFromRight(400));
        circuit.setBounds(r.reduced(6));
    }

private:
    CircuitView circuit;
    juce::GenericAudioProcessorEditor knobs;
};
