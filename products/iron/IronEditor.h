#pragma once

#include "IronProcessor.h"
#include "IronViews.h"

// Iron editor: the stage schematic (with the transformer drawn as its core)
// beside the live B-H loop, the iron knobs underneath, level knobs apart.
// Designed on a 1000x640 canvas and scaled as a whole.
class IronEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit IronEditor(IronProcessor& p)
        : AudioProcessorEditor(p), processor(p), schematic(p.engine), loop(p.engine.trace)
    {
        setLookAndFeel(&lnf);
        addAndMakeVisible(schematic);
        loop.valueFont = [](float h) { return pf::look::value(h); };
        loop.panelFont = [](float h) { return pf::look::panel(h); };
        addAndMakeVisible(loop);

        using namespace iron::look;
        addKnob(iron::kDrive, [](double v) { return copper.interpolatedWith(hot, (float) v); });
        addKnob(iron::kGap, [](double) { return silk; });
        addKnob(iron::kCore, [](double) { return steel; });
        addKnob(iron::kSteel, [](double v) { return steel.interpolatedWith(rust, (float) v); });
        addKnob(iron::kOutput, [](double) { return silk; });
        addKnob(iron::kMix, [](double) { return silk; });

        coldStart.setButtonText("Cold start");
        coldStart.setTooltip("Power the stage off and back on: hear the thump, and the core settles with a different remanent flux");
        coldStart.setColour(juce::TextButton::buttonColourId, copper.withAlpha(0.25f));
        coldStart.setColour(juce::TextButton::textColourOffId, silk);
        coldStart.onClick = [this] { processor.engine.coldStart(); };
        addAndMakeVisible(coldStart);

        for (int i = 0; i < p.getNumPrograms(); ++i) presets.addItem(p.getProgramName(i), i + 1);
        presets.setTextWhenNothingSelected("Presets");
        presets.setTitle("Preset");
        presets.onChange = [this] {
            if (presets.getSelectedId() > 0) processor.setCurrentProgram(presets.getSelectedId() - 1);
        };
        addAndMakeVisible(presets);

        setResizable(true, true);
        setResizeLimits(700, 448, 2000, 1280);
        getConstrainer()->setFixedAspectRatio(kW / kH);
        setSize((int) kW, (int) kH);
        startTimerHz(10);
    }

    ~IronEditor() override
    {
        for (auto& k : knobs) k.slider.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace iron::look;
        g.drawImageAt(background, 0, 0);
        const float s = scale();
        g.setFont(pf::look::panel(30 * s));
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.drawText("Iron", juce::Rectangle<float>(32, 18, 200, 40).translated(0, 1.5f) * s, juce::Justification::centredLeft);
        g.setColour(silk);
        g.drawText("Iron", juce::Rectangle<float>(32, 18, 200, 40) * s, juce::Justification::centredLeft);

        // recessed panels around the two views
        for (auto* c : { (juce::Component*) &schematic, (juce::Component*) &loop }) {
            const auto b = c->getBounds().toFloat();
            g.setColour(juce::Colours::black.withAlpha(0.5f));
            g.fillRoundedRectangle(b.expanded(5 * s), 8 * s);
            g.setColour(steel.withAlpha(0.18f));
            g.drawRoundedRectangle(b.expanded(5.5f * s).translated(0, 1.5f * s), 8 * s, 1.2f * s);
        }
        // divider between the iron knobs and the level knobs
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.drawLine(598 * s, 480 * s, 598 * s, 620 * s, 1.5f * s);
        g.setColour(steel.withAlpha(0.2f));
        g.drawLine(599.5f * s, 480 * s, 599.5f * s, 620 * s, 1.0f * s);

        for (auto& k : knobs) {
            const auto b = k.slider.getBounds().toFloat();
            const auto name = juce::Rectangle<float>(b.getX() - 30 * s, b.getBottom() + 2 * s, b.getWidth() + 60 * s, 18 * s);
            g.setFont(pf::look::panel(12.5f * s));
            g.setColour(silk.withAlpha(0.92f));
            g.drawText(iron::kKnobs[k.index].name, name, juce::Justification::centred);
            g.setFont(pf::look::value(16 * s));
            g.setColour(silk.withAlpha(0.6f));
            g.drawText(valueText(k.index, k.slider.getValue()), name.translated(0, 19 * s), juce::Justification::centred);
        }
    }

    void resized() override
    {
        const float s = scale();
        background = makeBackground(getWidth(), getHeight());
        schematic.setBounds((juce::Rectangle<float>(28, 76, 580, 380) * s).toNearestInt());
        loop.setBounds((juce::Rectangle<float>(628, 76, 344, 380) * s).toNearestInt());
        coldStart.setBounds((juce::Rectangle<float>(150, 24, 130, 30) * s).toNearestInt());
        presets.setBounds((juce::Rectangle<float>(772, 22, 200, 32) * s).toNearestInt());
        const float centres[] = { 90, 220, 350, 480, 730, 870 };   // drive gap core steel | output mix
        for (size_t i = 0; i < knobs.size(); ++i) {
            const float d = 84;
            knobs[i].slider.setBounds((juce::Rectangle<float>(centres[i] - d / 2, 480, d, d) * s).toNearestInt());
        }
    }

private:
    static constexpr float kW = 1000, kH = 640;

    struct Knob {
        int index;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    IronProcessor& processor;
    pf::look::LookAndFeel lnf;
    iron::SchematicView schematic;
    cd::ui::BHLoopView loop;
    std::array<Knob, iron::kNumKnobs> knobs;
    int knobCount = 0;
    juce::TextButton coldStart;
    juce::ComboBox presets;
    juce::Image background;

    float scale() const { return (float) getWidth() / kW; }

    // gunmetal with a faint lamination grain
    static juce::Image makeBackground(int w, int h)
    {
        juce::Image img(juce::Image::RGB, juce::jmax(1, w), juce::jmax(1, h), true);
        juce::Graphics g(img);
        iron::look::laminate(g, img.getBounds().toFloat(), iron::look::panel, 4.0f);
        juce::ColourGradient light(juce::Colours::white.withAlpha(0.07f), 0, 0,
                                   juce::Colours::black.withAlpha(0.3f), 0, (float) h, false);
        g.setGradientFill(light);
        g.fillAll();
        return img;
    }

    void addKnob(int index, pf::look::RingColour ring)
    {
        auto& k = knobs[(size_t) knobCount++];
        k.index = index;
        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        k.slider.setRotaryParameters(juce::degreesToRadians(-135.0f), juce::degreesToRadians(135.0f), true);
        k.slider.setTitle(iron::kKnobs[index].name);
        k.slider.setDoubleClickReturnValue(true, iron::kKnobs[index].def);
        k.slider.setLookAndFeel(&lnf);
        k.slider.onValueChange = [this] { repaint(); };
        lnf.setRing(k.slider, std::move(ring));
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, iron::kKnobs[index].id, k.slider);
        addAndMakeVisible(k.slider);
    }

    static juce::String valueText(int index, double v)
    {
        switch (index) {
        case iron::kDrive:
        case iron::kOutput: return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
        case iron::kGap: return juce::String(v, 2) + " mm";
        case iron::kCore:
        case iron::kMix: return juce::String(juce::roundToInt(v)) + "%";
        case iron::kSteel: return v < 15 ? "premium" : v < 60 ? "silicon" : "scrap";
        default: return juce::String(v, 2);
        }
    }

    void timerCallback() override
    {
        if (presets.getSelectedId() != processor.getCurrentProgram() + 1)
            presets.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);
    }
};
