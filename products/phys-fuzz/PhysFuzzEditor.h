#pragma once

#include "PhysFuzzProcessor.h"
#include "ui/CircuitView.h"

// The circuit on the left, showing what the macro knobs actually do to the
// components; the player's knobs on the right. (Placeholder look: generic sliders.)
class PhysFuzzEditor : public juce::AudioProcessorEditor {
public:
    explicit PhysFuzzEditor(PhysFuzzProcessor& p)
        : AudioProcessorEditor(p), circuit(p.engine), knobs(p)
    {
        addAndMakeVisible(circuit);
        addAndMakeVisible(knobs);
        setResizable(true, true);
        setResizeLimits(760, 420, 2400, 1400);
        setSize(1100, 600);
    }

    void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1b1c1e)); }

    void resized() override
    {
        auto r = getLocalBounds();
        knobs.setBounds(r.removeFromRight(360));
        circuit.setBounds(r.reduced(6));
    }

private:
    CircuitView circuit;
    juce::GenericAudioProcessorEditor knobs;
};
