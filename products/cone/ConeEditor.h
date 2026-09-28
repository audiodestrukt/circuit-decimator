#pragma once

#include "ConeProcessor.h"
#include "phys-fuzz/PhysFuzzLook.h"
#include "ui/SpeakerView.h"

// Cone editor: the speaker cab's live cross-section (drawn to scale from the
// knobs, the cone's motion at the cursor frequency, the response at the mic) on
// top; the speaker and cabinet knobs, then the mic and output, below.
// Designed on a 1000x720 canvas and scaled as a whole.
class ConeEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit ConeEditor(ConeProcessor& p)
        : AudioProcessorEditor(p), processor(p), view([&p] { return p.engine.cab(); })
    {
        setLookAndFeel(&lnf);
        addAndMakeVisible(view);

        const auto warm = juce::Colour(0xffc8703a), hot = juce::Colour(0xfff2b27a), silk = juce::Colour(0xffe6e2da);
        auto ramp = [=](double v) { return warm.interpolatedWith(hot, (float) v); };
        auto plain = [=](double) { return silk; };
        for (int k : { cone::kSize, cone::kPaper, cone::kWeight, cone::kDepth, cone::kDustCap, cone::kDamping })
            addKnob(k, ramp);
        for (int k : { cone::kVolume, cone::kOpening }) addKnob(k, ramp);
        for (int k : { cone::kMicPos, cone::kMicDist, cone::kMicAngle }) addKnob(k, ramp);
        addKnob(cone::kPower, ramp);
        for (int k : { cone::kLevel, cone::kMix }) addKnob(k, plain);

        for (int i = 0; i < 3; ++i) mic.addItem(cone::kMicNames[i], i + 1);
        mic.setTitle("Mic");
        addAndMakeVisible(mic);
        micAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.apvts, "mictype", mic);
        autoLevel.setButtonText("Auto Level");
        autoLevel.setTooltip("Hold the broadband level while the mic, box or speaker changes, so you hear the tone change");
        addAndMakeVisible(autoLevel);
        autoAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.apvts, "autolevel", autoLevel);
        heating.setButtonText("Coil Heating");
        heating.setTooltip("The voice coil warms as you play loud and its resistance rises: slow power compression (seconds)");
        addAndMakeVisible(heating);
        heatAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.apvts, "heating", heating);

        for (int i = 0; i < p.getNumPrograms(); ++i) presets.addItem(p.getProgramName(i), i + 1);
        presets.setTextWhenNothingSelected("Presets");
        presets.setTitle("Preset");
        presets.onChange = [this] {
            if (presets.getSelectedId() > 0) processor.setCurrentProgram(presets.getSelectedId() - 1);
        };
        addAndMakeVisible(presets);

        setResizable(true, true);
        setResizeLimits(700, 504, 2000, 1440);
        getConstrainer()->setFixedAspectRatio(kW / kH);
        setSize((int) kW, (int) kH);
        startTimerHz(10);
    }

    ~ConeEditor() override
    {
        for (auto& k : knobs) k.slider.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        const auto panel = juce::Colour(0xff2b2f34), silk = juce::Colour(0xffe6e2da), dim = juce::Colour(0xff8d949c);
        g.fillAll(panel);
        const float s = scale();
        juce::ColourGradient light(juce::Colours::white.withAlpha(0.06f), 0, 0, juce::Colours::black.withAlpha(0.3f), 0, (float) getHeight(), false);
        g.setGradientFill(light);
        g.fillAll();
        g.setFont(pf::look::panel(30 * s));
        g.setColour(silk);
        g.drawText("Cone", juce::Rectangle<float>(28, 14, 200, 40) * s, juce::Justification::centredLeft);
        g.setFont(pf::look::panel(13 * s));
        g.setColour(dim);
        g.drawText("SPEAKER + CABINET, PHYSICALLY MODELLED", juce::Rectangle<float>(126, 24, 420, 24) * s, juce::Justification::centredLeft);

        // group labels
        g.setFont(pf::look::panel(12 * s));
        auto group = [&](const char* t, float x0, float x1, float y) {
            g.setColour(dim);
            g.drawText(t, juce::Rectangle<float>(x0, y, x1 - x0, 14) * s, juce::Justification::centred);
            g.setColour(dim.withAlpha(0.35f));
            g.drawLine(x0 * s, (y + 16) * s, x1 * s, (y + 16) * s, 1.0f);
        };
        group("SPEAKER", 24, 640, 452);
        group("CABINET", 660, 976, 452);
        group("MICROPHONE", 24, 520, 582);
        group("DRIVE + OUTPUT", 540, 976, 582);

        for (auto& k : knobs) {
            const auto b = k.slider.getBounds().toFloat();
            const auto name = juce::Rectangle<float>(b.getX() - 30 * s, b.getBottom() + 1 * s, b.getWidth() + 60 * s, 15 * s);
            g.setFont(pf::look::panel(11.5f * s));
            g.setColour(silk.withAlpha(0.92f));
            g.drawText(juce::String(cone::kKnobs[k.index].name).toUpperCase(), name, juce::Justification::centred);
            g.setFont(pf::look::value(13 * s));
            g.setColour(silk.withAlpha(0.6f));
            g.drawText(valueText(k.index, k.slider.getValue()), name.translated(0, 14 * s), juce::Justification::centred);
        }
    }

    void resized() override
    {
        const float s = scale();
        auto at = [s](float x, float y, float w, float h) { return (juce::Rectangle<float>(x, y, w, h) * s).toNearestInt(); };
        view.setBounds(at(24, 62, 952, 380));
        presets.setBounds(at(756, 20, 220, 30));
        const float d = 62;
        // row 1: speaker (6) | cabinet (2)
        const float row1 = 474, row2 = 604;
        int i = 0;
        for (float cx : { 76.0f, 180.0f, 284.0f, 388.0f, 492.0f, 596.0f, 740.0f, 896.0f })
            knobs[(size_t) i++].slider.setBounds(at(cx - d / 2, row1, d, d));
        // row 2: mic position, distance, angle | output level, mix
        for (float cx : { 76.0f, 180.0f, 284.0f })
            knobs[(size_t) i++].slider.setBounds(at(cx - d / 2, row2, d, d));
        mic.setBounds(at(346, row2 + 16, 164, 28));
        heating.setBounds(at(546, row2 + 2, 140, 26));
        autoLevel.setBounds(at(546, row2 + 34, 140, 26));
        for (float cx : { 730.0f, 830.0f, 924.0f })
            knobs[(size_t) i++].slider.setBounds(at(cx - d / 2, row2, d, d));
    }

private:
    static constexpr float kW = 1000, kH = 720;

    struct Knob {
        int index;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    ConeProcessor& processor;
    pf::look::LookAndFeel lnf;
    cd::ui::SpeakerView view;
    std::array<Knob, 14> knobs;
    int knobCount = 0;
    juce::ComboBox mic, presets;
    juce::ToggleButton autoLevel, heating;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> micAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> autoAttachment, heatAttachment;

    float scale() const { return (float) getWidth() / kW; }

    void addKnob(int index, pf::look::RingColour ring)
    {
        auto& k = knobs[(size_t) knobCount++];
        k.index = index;
        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        k.slider.setRotaryParameters(juce::degreesToRadians(-135.0f), juce::degreesToRadians(135.0f), true);
        k.slider.setTitle(cone::kKnobs[index].name);
        k.slider.setDoubleClickReturnValue(true, cone::kKnobs[index].def);
        k.slider.setLookAndFeel(&lnf);
        k.slider.onValueChange = [this] { repaint(); };
        lnf.setRing(k.slider, std::move(ring));
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, cone::kKnobs[index].id, k.slider);
        addAndMakeVisible(k.slider);
    }

    static juce::String valueText(int index, double v)
    {
        const auto& k = cone::kKnobs[index];
        juce::String t = juce::String(v, k.decimals);
        if (index == cone::kLevel && v > 0) t = "+" + t;
        return std::strlen(k.unit) ? t + " " + k.unit : t;
    }

    void timerCallback() override
    {
        if (presets.getSelectedId() != processor.getCurrentProgram() + 1)
            presets.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);
    }
};
