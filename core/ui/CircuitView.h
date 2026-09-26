#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/FuzzEngine.h"

// Live schematic of the Fuzz Face. Wire colour = node DC voltage (0 V blue ->
// battery amber), wire glow = how much signal swings on that node, component
// colour = how far its knob is from healthy (red = damaged). Leakage paths
// appear as dashed resistors once their knob is up. Reads the engine's
// per-block telemetry (node voltages + the knob values the circuit is really
// using, so macro knobs show their effect) on a timer; never touches the audio thread.
class CircuitView : public juce::Component, private juce::Timer {
public:
    // Schematic: utilitarian engineering view (voltage colours, all readouts).
    // Board: the product look -- copper traces on phenolic board, silkscreen
    // symbols, signal brightening the copper, patina turning it verdigris.
    enum class Style { Schematic, Board };

    explicit CircuitView(cd::FuzzEngine&);
    void setStyle(Style);
    void setPatina(float amount);                    // 0 = new copper, 1 = verdigris (Board)
    void setTypeface(juce::Typeface::Ptr);          // labels; default font if null
    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    using Pt = juce::Point<float>;
    using Node = cd::FuzzFace::Node;
    static constexpr int GND = -1;

    struct Knobs {
        double v[cd::kNumKnobs];
        double operator[](int i) const { return v[i]; }
    };

    juce::Colour voltageColour(float volts) const;
    float swing(int node) const;
    float mean(int node) const;

    // drawing primitives, in virtual canvas coordinates
    void wire(juce::Graphics&, std::initializer_list<Pt>, int node) const;
    void dot(juce::Graphics&, Pt, int node) const;
    void ground(juce::Graphics&, Pt) const;
    void resistor(juce::Graphics&, Pt a, Pt b, float damage, float alpha = 1, bool dashed = false) const;
    void capacitor(juce::Graphics&, Pt a, Pt b, float damage) const;
    void inductor(juce::Graphics&, Pt a, Pt b) const;
    void transistor(juce::Graphics&, Pt base, float damage, float tempC) const;
    void battery(juce::Graphics&, Pt top, Pt bottom, float damage) const;
    void wiper(juce::Graphics&, Pt tip, Pt from, float damage) const;
    void label(juce::Graphics&, const juce::String&, Pt, float size = 13, juce::Colour = {},
               juce::Justification = juce::Justification::centredLeft) const;
    juce::Colour partColour(float damage) const;
    juce::Colour traceColour(int node) const;       // Board: copper/verdigris, brighter with signal
    juce::Colour groundColour() const;
    juce::Colour textColour() const;
    bool board() const { return style == Style::Board; }

    cd::FuzzEngine& engine;
    Knobs knobs {};
    std::array<float, cd::FuzzEngine::kNodes> vMean {}, vSwing {};
    float scale = 1;
    Style style = Style::Schematic;
    float patina = 0;
    juce::Typeface::Ptr typeface;
    juce::Image boardTexture;   // cached phenolic background for Board style
};
