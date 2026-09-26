// Phys Fuzz look: hammertone enclosure, bakelite knobs whose skirt ring shows
// what the knob is doing (charge left, copper turning to verdigris, ice to
// heat), Michroma panel lettering and Barlow for values.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "BinaryData.h"

namespace pf::look {

inline const juce::Colour hammertone { 0xff2e4a48 };
inline const juce::Colour hammertoneLight { 0xff4f7571 };
inline const juce::Colour bakelite { 0xff16100c };
inline const juce::Colour copper { 0xffe0915a };
inline const juce::Colour copperHot { 0xfff6c79a };
inline const juce::Colour verdigris { 0xff6fb8a0 };
inline const juce::Colour silk { 0xffe9e4d8 };
inline const juce::Colour scorch { 0xffff5a36 };
inline const juce::Colour ice { 0xff7fc8ff };
inline const juce::Colour heat { 0xffff8a3d };

inline juce::Typeface::Ptr panelFace()
{
    static auto t = juce::Typeface::createSystemTypefaceFor(BinaryData::MichromaRegular_ttf,
                                                           BinaryData::MichromaRegular_ttfSize);
    return t;
}

inline juce::Typeface::Ptr valueFace()
{
    static auto t = juce::Typeface::createSystemTypefaceFor(BinaryData::BarlowSemiBold_ttf,
                                                           BinaryData::BarlowSemiBold_ttfSize);
    return t;
}

inline juce::Font panel(float height) { return juce::Font(juce::FontOptions(panelFace()).withHeight(height)); }
inline juce::Font value(float height) { return juce::Font(juce::FontOptions(valueFace()).withHeight(height)); }

// How a knob's skirt ring is coloured for a value 0..1 (set per slider).
using RingColour = std::function<juce::Colour(double proportion)>;

class LookAndFeel : public juce::LookAndFeel_V4 {
public:
    LookAndFeel()
    {
        setColour(juce::ComboBox::backgroundColourId, bakelite.withAlpha(0.6f));
        setColour(juce::ComboBox::textColourId, silk);
        setColour(juce::ComboBox::outlineColourId, silk.withAlpha(0.25f));
        setColour(juce::ComboBox::arrowColourId, copper);
        setColour(juce::ComboBox::focusedOutlineColourId, copper);
        setColour(juce::PopupMenu::backgroundColourId, juce::Colour(0xff1f2f2e));
        setColour(juce::PopupMenu::textColourId, silk);
        setColour(juce::PopupMenu::highlightedBackgroundColourId, copper.withAlpha(0.85f));
        setColour(juce::PopupMenu::highlightedTextColourId, bakelite);
        setColour(juce::PopupMenu::headerTextColourId, copper);
    }

    juce::Font getComboBoxFont(juce::ComboBox& box) override { return value((float) box.getHeight() * 0.55f); }
    juce::Font getPopupMenuFont() override { return value(17.0f); }

    void drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end,
                          juce::Slider& s) override
    {
        const auto area = juce::Rectangle<float>((float) x, (float) y, (float) w, (float) h).reduced(4);
        const float r = juce::jmin(area.getWidth(), area.getHeight()) / 2;
        const auto c = area.getCentre();
        const float angle = start + pos * (end - start);

        // skirt: engraved track + the value ring in this knob's colour
        juce::Path track;
        track.addCentredArc(c.x, c.y, r * 0.92f, r * 0.92f, 0, start, end, true);
        g.setColour(juce::Colours::black.withAlpha(0.35f));
        g.strokePath(track, juce::PathStrokeType(r * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        auto ringFn = ringColours.find(&s);
        const auto ring = ringFn != ringColours.end() ? ringFn->second(pos) : copper;
        juce::Path arc;
        arc.addCentredArc(c.x, c.y, r * 0.92f, r * 0.92f, 0, start, angle, true);
        g.setColour(ring);
        g.strokePath(arc, juce::PathStrokeType(r * 0.09f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // tick marks round the skirt, like a printed pedal scale
        g.setColour(silk.withAlpha(0.35f));
        for (int i = 0; i <= 10; ++i) {
            const float a = start + (end - start) * (float) i / 10.0f;
            const auto dir = juce::Point<float>(std::sin(a), -std::cos(a));
            g.drawLine({ c + dir * r * 1.02f, c + dir * r * (i % 5 == 0 ? 1.12f : 1.08f) }, i % 5 == 0 ? 1.6f : 1.0f);
        }

        // bakelite knob body: shadow, knurled edge, domed top
        const float kr = r * 0.72f;
        g.setColour(juce::Colours::black.withAlpha(0.45f));
        g.fillEllipse(c.x - kr + kr * 0.06f, c.y - kr + kr * 0.12f, kr * 2, kr * 2);
        juce::Path knurl;
        for (int i = 0; i < 48; ++i) {
            const float a = juce::MathConstants<float>::twoPi * (float) i / 48.0f;
            const float rr = (i % 2) ? kr : kr * 0.965f;
            const auto p = c + juce::Point<float>(std::sin(a), -std::cos(a)) * rr;
            if (i == 0) knurl.startNewSubPath(p); else knurl.lineTo(p);
        }
        knurl.closeSubPath();
        g.setColour(bakelite);
        g.fillPath(knurl);
        juce::ColourGradient dome(juce::Colour(0xff4a3a30), c.x - kr * 0.35f, c.y - kr * 0.45f,
                                  bakelite, c.x + kr * 0.3f, c.y + kr * 0.5f, true);
        g.setGradientFill(dome);
        g.fillEllipse(c.x - kr * 0.8f, c.y - kr * 0.8f, kr * 1.6f, kr * 1.6f);

        // pointer: silkscreen line from the centre towards the edge
        const auto dir = juce::Point<float>(std::sin(angle), -std::cos(angle));
        g.setColour(silk);
        g.drawLine({ c + dir * kr * 0.25f, c + dir * kr * 0.9f }, kr * 0.09f);

        if (s.hasKeyboardFocus(true)) {
            g.setColour(copper.withAlpha(0.8f));
            g.drawEllipse(area.withSizeKeepingCentre(r * 2.3f, r * 2.3f), 1.5f);
        }
    }

    void setRing(juce::Slider& s, RingColour fn) { ringColours[&s] = std::move(fn); }

private:
    std::map<juce::Slider*, RingColour> ringColours;
};

// Hammertone paint: speckled enamel with a soft top light. Rendered once per size.
inline juce::Image hammertonePanel(int w, int h)
{
    juce::Image img(juce::Image::RGB, juce::jmax(1, w), juce::jmax(1, h), true);
    juce::Graphics g(img);
    g.fillAll(hammertone);
    juce::Random rng(1971);
    const int dimples = w * h / 90;
    for (int i = 0; i < dimples; ++i) {
        const float x = rng.nextFloat() * (float) w, y = rng.nextFloat() * (float) h;
        const float s = 2.0f + rng.nextFloat() * 5.0f;
        g.setColour((rng.nextBool() ? hammertoneLight : juce::Colours::black).withAlpha(0.06f + 0.08f * rng.nextFloat()));
        g.fillEllipse(x, y, s, s * 0.8f);
    }
    juce::ColourGradient light(juce::Colours::white.withAlpha(0.10f), (float) w * 0.3f, 0,
                               juce::Colours::black.withAlpha(0.30f), (float) w * 0.7f, (float) h, false);
    g.setGradientFill(light);
    g.fillAll();
    return img;
}

} // namespace pf::look
