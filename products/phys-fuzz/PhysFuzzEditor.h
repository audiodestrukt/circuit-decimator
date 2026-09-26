#pragma once

#include "PhysFuzzLook.h"
#include "PhysFuzzProcessor.h"
#include "ui/CircuitView.h"

// Phys Fuzz editor: the pedal with its lid off. The circuit board is the
// window (copper brightens with signal, Age turns it verdigris). The knobs
// that change the circuit's condition (Battery, Age, Temperature) sit under
// the board; the playing knobs (Fuzz, Volume) and the level (Output) run down
// the right like a pedal's control column. The circuit selector (Fuzz Face /
// Shin-Ei FY-2) sits above the board: the board redraws as that pedal.
// Designed on a 960x640 canvas and scaled as a whole.
class PhysFuzzEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PhysFuzzEditor(PhysFuzzProcessor& p)
        : AudioProcessorEditor(p), processor(p), circuit(p.engine)
    {
        setLookAndFeel(&lnf);
        circuit.setStyle(CircuitView::Style::Board);
        circuit.setTypeface(pf::look::valueFace());
        addAndMakeVisible(circuit);

        using namespace pf::look;
        addKnob(pf::kFuzz, [](double) { return copper; });
        addKnob(pf::kVolume, [](double) { return silk; });
        addKnob(pf::kBattery, [](double v) { return scorch.interpolatedWith(copper, (float) v); });
        addKnob(pf::kAge, [](double v) { return copper.interpolatedWith(verdigris, (float) v); });
        addKnob(pf::kTemperature, [](double v) {
            const double c = -20 + 100 * v;   // knob range, degrees C
            return c < 27 ? ice.interpolatedWith(silk, (float) ((c + 20) / 47)) : silk.interpolatedWith(heat, (float) ((c - 27) / 53));
        });
        addKnob(pf::kOutput, [](double) { return silk; });

        for (int i = 0; i < p.getNumPrograms(); ++i) presets.addItem(p.getProgramName(i), i + 1);
        presets.setTextWhenNothingSelected("Presets");
        presets.setTitle("Preset");
        presets.onChange = [this] {
            if (presets.getSelectedId() > 0) processor.setCurrentProgram(presets.getSelectedId() - 1);
        };
        addAndMakeVisible(presets);

        for (int i = 0; i < cd::FuzzEngine::kNumModels; ++i)
            circuits.addItem(cd::FuzzEngine::modelName((cd::FuzzEngine::Model) i), i + 1);
        circuits.setTitle("Circuit");
        circuitAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
            processor.apvts, "circuit", circuits);
        addAndMakeVisible(circuits);

        setResizable(true, true);
        setResizeLimits(672, 448, 1920, 1280);
        getConstrainer()->setFixedAspectRatio(kW / kH);
        setSize((int) kW, (int) kH);
        startTimerHz(30);
    }

    ~PhysFuzzEditor() override
    {
        for (auto& k : knobs) k.slider.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        g.drawImageAt(panel, 0, 0);
        const float s = scale();
        using namespace pf::look;

        // engraved product name
        const auto title = juce::Rectangle<float>(32, 16, 400, 40) * s;
        g.setFont(pf::look::panel(26 * s));
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.drawText("Phys Fuzz", title.translated(0, 1.5f * s), juce::Justification::centredLeft);
        g.setColour(silk);
        g.drawText("Phys Fuzz", title, juce::Justification::centredLeft);

        // bezel around the circuit window: dark recess with a lit lower lip
        const auto win = windowBounds().toFloat();
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(win.expanded(6 * s), 10 * s);
        g.setColour(hammertoneLight.withAlpha(0.35f));
        g.drawRoundedRectangle(win.expanded(6.5f * s).translated(0, 1.5f * s), 10 * s, 1.2f * s);

        // engraved dividers: board | control column, and playing knobs | level
        auto engrave = [&](float x1, float y1, float x2, float y2) {
            g.setColour(juce::Colours::black.withAlpha(0.35f));
            g.drawLine(x1 * s, y1 * s, x2 * s, y2 * s, 1.5f * s);
            g.setColour(hammertoneLight.withAlpha(0.4f));
            g.drawLine((x1 + 1.5f) * s, (y1 + 1.5f) * s, (x2 + 1.5f) * s, (y2 + 1.5f) * s, 1.0f * s);
        };
        engrave(716, 76, 716, 628);
        engrave(744, 474, 928, 474);

        // knob names (panel lettering) and values
        for (auto& k : knobs) {
            const auto b = k.slider.getBounds().toFloat();
            const auto name = juce::Rectangle<float>(b.getX() - 30 * s, b.getBottom() + 2 * s, b.getWidth() + 60 * s, 18 * s);
            g.setFont(pf::look::panel(12.5f * s));
            g.setColour(silk.withAlpha(0.92f));
            g.drawText(pf::kKnobs[k.index].name, name, juce::Justification::centred);
            g.setFont(pf::look::value(16 * s));
            g.setColour(silk.withAlpha(0.6f));
            g.drawText(valueText(k.index, k.slider.getValue()), name.translated(0, 19 * s), juce::Justification::centred);
        }
    }

    void resized() override
    {
        const float s = scale();
        panel = pf::look::hammertonePanel(getWidth(), getHeight());
        circuit.setBounds(windowBounds());
        presets.setBounds((juce::Rectangle<float>(740, 22, 192, 32) * s).toNearestInt());
        circuits.setBounds((juce::Rectangle<float>(492, 22, 200, 32) * s).toNearestInt());
        // knob centres, in knobs[] order: fuzz, volume, battery, age, temperature, output
        const juce::Point<float> centres[] = { { 836, 138 }, { 836, 320 }, { 150, 540 },
                                               { 360, 540 }, { 570, 540 }, { 836, 540 } };
        for (size_t i = 0; i < knobs.size(); ++i) {
            const float d = 92;
            knobs[i].slider.setBounds((juce::Rectangle<float>(d, d).withCentre(centres[i]) * s).toNearestInt());
        }
    }

private:
    static constexpr float kW = 960, kH = 640;

    struct Knob {
        int index;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    PhysFuzzProcessor& processor;
    pf::look::LookAndFeel lnf;
    CircuitView circuit;
    std::array<Knob, pf::kNumKnobs> knobs;
    int knobCount = 0;
    juce::ComboBox presets, circuits;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> circuitAttachment;
    juce::Image panel;

    float scale() const { return (float) getWidth() / kW; }

    juce::Rectangle<int> windowBounds() const
    {
        // the schematic's own 1000:620 proportions, so the board fills the window
        return (juce::Rectangle<float>(28, 72, 664, 412) * scale()).toNearestInt();
    }

    void addKnob(int index, pf::look::RingColour ring)
    {
        auto& k = knobs[(size_t) knobCount++];
        k.index = index;
        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        k.slider.setRotaryParameters(juce::degreesToRadians(-135.0f), juce::degreesToRadians(135.0f), true);
        k.slider.setTitle(pf::kKnobs[index].name);
        k.slider.setDoubleClickReturnValue(true, pf::kKnobs[index].def);
        k.slider.setLookAndFeel(&lnf);
        k.slider.onValueChange = [this] { repaint(); };
        lnf.setRing(k.slider, std::move(ring));
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, pf::kKnobs[index].id, k.slider);
        addAndMakeVisible(k.slider);
    }

    static juce::String valueText(int index, double v)
    {
        switch (index) {
        case pf::kFuzz:
        case pf::kVolume: return juce::String(v * 10, 1);   // pedal-style 0..10
        case pf::kBattery:
        case pf::kAge: return juce::String(juce::roundToInt(v)) + "%";
        case pf::kTemperature: return juce::String(juce::roundToInt(v)) + juce::String::fromUTF8(" \xc2\xb0" "C");
        case pf::kOutput: return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
        default: return juce::String(v, 2);
        }
    }

    void timerCallback() override
    {
        // Age oxidises the copper on the board
        circuit.setPatina(0.85f * (float) processor.apvts.getRawParameterValue("age")->load() / 100.0f);
        if (presets.getSelectedId() != processor.getCurrentProgram() + 1)
            presets.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);
    }
};
