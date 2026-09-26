#pragma once

#include "OptoProcessor.h"
#include "OptoViews.h"

// Opto editor: the LA-2A-style front panel. The meter (gain reduction or
// output) in the middle, the T4 cell on the left, recent gain reduction on the
// right, Peak Reduction and Gain underneath with Mix between them.
// Designed on a 900x560 canvas and scaled as a whole.
class OptoEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit OptoEditor(OptoProcessor& p)
        : AudioProcessorEditor(p), processor(p), meter(p.engine), t4(p.engine), history(p.engine)
    {
        setLookAndFeel(&lnf);
        addAndMakeVisible(meter);
        addAndMakeVisible(t4);
        addAndMakeVisible(history);

        using namespace opto::look;
        const auto copper = juce::Colour(0xffc8703a), hot = amber;
        addKnob(opto::kPeak, [=](double v) { return copper.interpolatedWith(hot, (float) v); });
        addKnob(opto::kMix, [](double) { return silk; });
        addKnob(opto::kGain, [](double) { return silk; });

        for (auto* b : { &grButton, &outButton }) {
            b->setClickingTogglesState(true);
            b->setRadioGroupId(1);
            b->setColour(juce::TextButton::buttonColourId, window);
            b->setColour(juce::TextButton::buttonOnColourId, copper.withAlpha(0.55f));
            b->setColour(juce::TextButton::textColourOffId, dim);
            b->setColour(juce::TextButton::textColourOnId, silk);
            addAndMakeVisible(*b);
        }
        grButton.setToggleState(true, juce::dontSendNotification);
        grButton.onClick = [this] { meter.setMode(opto::VUMeter::GainReduction); };
        outButton.onClick = [this] { meter.setMode(opto::VUMeter::Output); };

        for (int i = 0; i < p.getNumPrograms(); ++i) presets.addItem(p.getProgramName(i), i + 1);
        presets.setTextWhenNothingSelected("Presets");
        presets.setTitle("Preset");
        presets.onChange = [this] {
            if (presets.getSelectedId() > 0) processor.setCurrentProgram(presets.getSelectedId() - 1);
        };
        addAndMakeVisible(presets);

        setResizable(true, true);
        setResizeLimits(630, 392, 1800, 1120);
        getConstrainer()->setFixedAspectRatio(kW / kH);
        setSize((int) kW, (int) kH);
        startTimerHz(10);
    }

    ~OptoEditor() override
    {
        for (auto& k : knobs) k.slider.setLookAndFeel(nullptr);
        setLookAndFeel(nullptr);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace opto::look;
        g.drawImageAt(background, 0, 0);
        const float s = scale();
        auto title = juce::Rectangle<float>(32, 16, 300, 40);
        g.setFont(pf::look::panel(30 * s));
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.drawText("Opto", (title * s).translated(0, 1.5f), juce::Justification::centredLeft);
        g.setColour(silk);
        g.drawText("Opto", title * s, juce::Justification::centredLeft);
        g.setFont(pf::look::panel(13 * s));
        g.setColour(dim);
        g.drawText("OPTICAL LEVELING AMPLIFIER", juce::Rectangle<float>(122, 26, 300, 24) * s, juce::Justification::centredLeft);

        // screws in the corners of the faceplate
        for (auto p : { juce::Point<float>(16, 16), { kW - 16, 16 }, { 16, kH - 16 }, { kW - 16, kH - 16 } }) {
            const auto c = p * s;
            g.setColour(juce::Colours::black.withAlpha(0.4f));
            g.fillEllipse(c.x - 6 * s, c.y - 5 * s, 12 * s, 12 * s);
            g.setColour(juce::Colour(0xff9aa1a9));
            g.fillEllipse(c.x - 5 * s, c.y - 5 * s, 10 * s, 10 * s);
            g.setColour(juce::Colour(0xff4a5058));
            g.drawLine(c.x - 3.5f * s, c.y - 1.5f * s, c.x + 3.5f * s, c.y + 1.5f * s, 1.5f * s);
        }

        for (auto& k : knobs) {
            const auto b = k.slider.getBounds().toFloat();
            const auto name = juce::Rectangle<float>(b.getX() - 40 * s, b.getBottom() + 2 * s, b.getWidth() + 80 * s, 18 * s);
            g.setFont(pf::look::panel((k.index == opto::kMix ? 12.0f : 14.0f) * s));
            g.setColour(silk.withAlpha(0.92f));
            g.drawText(juce::String(opto::kKnobs[k.index].name).toUpperCase(), name, juce::Justification::centred);
            g.setFont(pf::look::value(15 * s));
            g.setColour(silk.withAlpha(0.6f));
            g.drawText(valueText(k.index, k.slider.getValue()), name.translated(0, 19 * s), juce::Justification::centred);
        }
    }

    void resized() override
    {
        const float s = scale();
        background = makeBackground(getWidth(), getHeight());
        auto at = [s](float x, float y, float w, float h) { return (juce::Rectangle<float>(x, y, w, h) * s).toNearestInt(); };
        t4.setBounds(at(36, 80, 160, 250));
        meter.setBounds(at(220, 72, 460, 250));
        history.setBounds(at(704, 80, 160, 250));
        grButton.setBounds(at(340, 334, 110, 26));
        outButton.setBounds(at(450, 334, 110, 26));
        presets.setBounds(at(664, 22, 200, 32));
        const float cx[] = { 220, 450, 680 }, d[] = { 124, 78, 124 }, y[] = { 380, 402, 380 };
        for (size_t i = 0; i < knobs.size(); ++i)
            knobs[i].slider.setBounds(at(cx[i] - d[i] / 2, y[i], d[i], d[i]));
    }

private:
    static constexpr float kW = 900, kH = 560;

    struct Knob {
        int index;
        juce::Slider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    OptoProcessor& processor;
    pf::look::LookAndFeel lnf;
    opto::VUMeter meter;
    opto::T4View t4;
    opto::HistoryView history;
    std::array<Knob, opto::kNumKnobs> knobs;
    int knobCount = 0;
    juce::TextButton grButton { "GAIN RED." }, outButton { "OUTPUT" };
    juce::ComboBox presets;
    juce::Image background;

    float scale() const { return (float) getWidth() / kW; }

    // brushed faceplate
    static juce::Image makeBackground(int w, int h)
    {
        juce::Image img(juce::Image::RGB, juce::jmax(1, w), juce::jmax(1, h), true);
        juce::Graphics g(img);
        g.fillAll(opto::look::faceplate);
        juce::Random rng(1967);
        for (int y = 0; y < h; ++y) {
            g.setColour((rng.nextBool() ? juce::Colours::white : juce::Colours::black).withAlpha(0.025f + 0.03f * rng.nextFloat()));
            g.drawHorizontalLine(y, 0, (float) w);
        }
        juce::ColourGradient light(juce::Colours::white.withAlpha(0.08f), 0, 0,
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
        k.slider.setTitle(opto::kKnobs[index].name);
        k.slider.setDoubleClickReturnValue(true, opto::kKnobs[index].def);
        k.slider.setLookAndFeel(&lnf);
        k.slider.onValueChange = [this] { repaint(); };
        lnf.setRing(k.slider, std::move(ring));
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
            processor.apvts, opto::kKnobs[index].id, k.slider);
        addAndMakeVisible(k.slider);
    }

    static juce::String valueText(int index, double v)
    {
        switch (index) {
        case opto::kPeak: return juce::String(juce::roundToInt(v));
        case opto::kGain: return (v > 0 ? "+" : "") + juce::String(v, 1) + " dB";
        case opto::kMix: return juce::String(juce::roundToInt(v)) + "%";
        default: return juce::String(v, 2);
        }
    }

    void timerCallback() override
    {
        if (presets.getSelectedId() != processor.getCurrentProgram() + 1)
            presets.setSelectedId(processor.getCurrentProgram() + 1, juce::dontSendNotification);
    }
};
