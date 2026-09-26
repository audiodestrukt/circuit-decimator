// Opto's live views, all reading the engine's telemetry on a timer:
//   VUMeter      the front-panel meter: gain reduction or output level
//   T4View       the opto cell: EL panel glowing with the sidechain's drive,
//                the photocell's resistance
//   HistoryView  the last few seconds of gain reduction
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "OptoEngine.h"
#include "phys-fuzz/PhysFuzzLook.h"

namespace opto::look {

inline const juce::Colour faceplate { 0xff3a3f45 };
inline const juce::Colour window { 0xff15181b };
inline const juce::Colour cream { 0xfff3e6c4 };
inline const juce::Colour ink { 0xff2b2723 };
inline const juce::Colour red { 0xffb8402c };
inline const juce::Colour silk { 0xffe6e2da };
inline const juce::Colour dim { 0xff8d949c };
inline const juce::Colour el { 0xff7ff0d8 };    // electroluminescent blue-green
inline const juce::Colour amber { 0xfff2b27a };

} // namespace opto::look

namespace opto {

// ------------------------------------------------------------------ VU meter
class VUMeter : public juce::Component, private juce::Timer {
public:
    enum Mode { GainReduction, Output };
    explicit VUMeter(Engine& e) : engine(e) { startTimerHz(40); }

    void setMode(Mode m) { mode = m; repaint(); }
    Mode getMode() const { return mode; }

    void paint(juce::Graphics& g) override
    {
        using namespace look;
        auto b = getLocalBounds().toFloat();
        // bezel
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        g.fillRoundedRectangle(b, 10);
        auto face = b.reduced(b.getHeight() * 0.05f);
        juce::ColourGradient lamp(cream.brighter(0.08f), face.getCentreX(), face.getY() + face.getHeight() * 0.2f,
                                  cream.darker(0.18f), face.getX(), face.getBottom(), true);
        g.setGradientFill(lamp);
        g.fillRoundedRectangle(face, 6);

        const float cx = face.getCentreX(), cy = face.getBottom() + face.getHeight() * 0.28f;
        const float rad = face.getHeight() * 0.98f;
        const float s = face.getHeight() / 200.0f;

        // scale arc: black to 0, red above
        juce::Path arcBlack, arcRed;
        arcBlack.addCentredArc(cx, cy, rad * 0.8f, rad * 0.8f, 0, angleFor(-20), angleFor(0), true);
        arcRed.addCentredArc(cx, cy, rad * 0.8f, rad * 0.8f, 0, angleFor(0), angleFor(3), true);
        g.setColour(ink);
        g.strokePath(arcBlack, juce::PathStrokeType(2.0f * s));
        g.setColour(red);
        g.strokePath(arcRed, juce::PathStrokeType(5.0f * s));

        g.setFont(pf::look::value(15 * s));
        for (int db : { -20, -10, -7, -5, -3, -2, -1, 0, 1, 2, 3 }) {
            const float a = angleFor((float) db);
            const auto dir = juce::Point<float>(std::sin(a), -std::cos(a));
            const auto c = juce::Point<float>(cx, cy);
            g.setColour(db > 0 ? red : ink);
            g.drawLine({ c + dir * rad * 0.8f, c + dir * rad * 0.88f }, 2.0f * s);
            const auto t = c + dir * rad * 0.97f;
            const juce::String txt = mode == GainReduction ? (db <= 0 ? juce::String(-db) : "")
                                                           : (db > 0 ? "+" + juce::String(db) : juce::String(db));
            g.drawText(txt, juce::Rectangle<float>(t.x - 20 * s, t.y - 9 * s, 40 * s, 18 * s), juce::Justification::centred);
        }
        g.setColour(ink);
        g.setFont(pf::look::panel(17 * s));
        g.drawText(mode == GainReduction ? "GAIN REDUCTION  dB" : "OUTPUT  VU",
                   face.withTrimmedTop(face.getHeight() * 0.7f).withTrimmedBottom(face.getHeight() * 0.1f),
                   juce::Justification::centred);

        // needle
        {
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(face.toNearestInt());
            const float a = angleFor(needle);
            const auto dir = juce::Point<float>(std::sin(a), -std::cos(a));
            const auto c = juce::Point<float>(cx, cy);
            g.setColour(juce::Colours::black.withAlpha(0.18f));
            g.drawLine({ c + juce::Point<float>(3 * s, 3 * s), c + dir * rad * 0.93f + juce::Point<float>(3 * s, 3 * s) }, 2.4f * s);
            g.setColour(ink);
            g.drawLine({ c, c + dir * rad * 0.93f }, 2.0f * s);
        }
        // glass sheen
        juce::ColourGradient glass(juce::Colours::white.withAlpha(0.16f), face.getX(), face.getY(),
                                   juce::Colours::transparentWhite, face.getX(), face.getCentreY(), false);
        g.setGradientFill(glass);
        g.fillRoundedRectangle(face, 6);
    }

private:
    Engine& engine;
    Mode mode = GainReduction;
    float needle = 0;   // VU dB

    // VU deflection is proportional to voltage: -20 at the left stop, +3 at the right
    static float angleFor(float db)
    {
        const float lo = 0.1f, hi = 1.4125f;
        const float p = (std::pow(10.0f, juce::jlimit(-24.0f, 4.0f, db) / 20.0f) - lo) / (hi - lo);
        return -0.85f + 1.7f * juce::jlimit(-0.05f, 1.03f, p);
    }

    void timerCallback() override
    {
        float target;
        if (mode == GainReduction) {
            target = -engine.gainReduction.load();
        } else {
            const float rms = engine.outputRms.load();
            target = rms > 1e-6f ? 20 * std::log10(rms) + 18.0f : -40.0f;   // 0 VU = -18 dBFS rms
        }
        if (!std::isfinite(target)) target = 0;
        needle += (target - needle) * 0.3f;   // ~300 ms ballistics at 40 Hz
        repaint();
    }
};

// ------------------------------------------------------------------ T4 cell
class T4View : public juce::Component, private juce::Timer {
public:
    explicit T4View(Engine& e) : engine(e) { startTimerHz(30); }

    void paint(juce::Graphics& g) override
    {
        using namespace look;
        auto b = getLocalBounds().toFloat();
        g.setColour(window);
        g.fillRoundedRectangle(b, 8);
        const float s = b.getWidth() / 160.0f;
        auto r = b.reduced(12 * s);
        g.setFont(pf::look::panel(14 * s));
        g.setColour(dim);
        g.drawText("T4 CELL", r.removeFromTop(18 * s), juce::Justification::centredLeft);

        // the canister, and inside it the EL panel facing the photocell
        auto can = r.removeFromTop(r.getHeight() * 0.72f).reduced(10 * s, 6 * s);
        g.setColour(juce::Colour(0xff2a2e33));
        g.fillRoundedRectangle(can, 12 * s);
        g.setColour(juce::Colour(0xff4a5058));
        g.drawRoundedRectangle(can, 12 * s, 1.5f * s);

        const float glow = juce::jlimit(0.0f, 1.0f, std::sqrt(juce::jmax(0.0f, light)) * 1.6f);
        auto panel = can.removeFromTop(can.getHeight() * 0.42f).reduced(10 * s, 8 * s);
        g.setColour(el.withAlpha(0.05f + 0.25f * glow));
        g.fillRoundedRectangle(panel.expanded(8 * s * glow), 6 * s);
        g.setColour(juce::Colour(0xff203a36).interpolatedWith(el, glow));
        g.fillRoundedRectangle(panel, 3 * s);
        g.setFont(pf::look::value(12 * s));
        g.setColour(juce::Colours::black.withAlpha(0.5f));
        g.drawText("EL panel", panel, juce::Justification::centred);

        // light falling on the cell
        auto cell = can.reduced(0, 6 * s).withSizeKeepingCentre(can.getHeight() * 0.62f, can.getHeight() * 0.62f);
        juce::ColourGradient beam(el.withAlpha(0.35f * glow), panel.getCentreX(), panel.getBottom(),
                                  el.withAlpha(0.0f), cell.getCentreX(), cell.getCentreY(), false);
        g.setGradientFill(beam);
        g.fillRect(juce::Rectangle<float>(cell.getX(), panel.getBottom(), cell.getWidth(), cell.getCentreY() - panel.getBottom()));
        g.setColour(juce::Colour(0xff8a6a3a));
        g.fillEllipse(cell);
        g.setColour(juce::Colour(0xffd2a45a).withAlpha(0.8f));   // CdS track
        juce::Path zig;
        const int n = 6;
        for (int i = 0; i <= n; ++i) {
            const float y = cell.getY() + cell.getHeight() * (0.22f + 0.56f * (float) i / n);
            const float x = (i % 2) ? cell.getX() + cell.getWidth() * 0.25f : cell.getRight() - cell.getWidth() * 0.25f;
            if (i == 0) zig.startNewSubPath(x, y); else zig.lineTo(x, y);
        }
        g.strokePath(zig, juce::PathStrokeType(1.6f * s));
        g.setColour(el.withAlpha(0.3f * glow));
        g.fillEllipse(cell);

        // readout
        g.setFont(pf::look::value(16 * s));
        g.setColour(silk);
        g.drawText(ohms(cell_), r, juce::Justification::centred);
    }

private:
    Engine& engine;
    float light = 0, cell_ = 5e6f;

    static juce::String ohms(float r)
    {
        if (r >= 1e6f) return juce::String(r / 1e6f, 2) + " M" + juce::String::fromUTF8("\xce\xa9");
        if (r >= 1e3f) return juce::String(r / 1e3f, r < 1e4f ? 2 : 1) + " k" + juce::String::fromUTF8("\xce\xa9");
        return juce::String(juce::roundToInt(r)) + " " + juce::String::fromUTF8("\xce\xa9");
    }

    void timerCallback() override
    {
        light += (engine.panelLight.load() - light) * 0.5f;
        cell_ = engine.cellOhms.load();
        repaint();
    }
};

// ------------------------------------------------------------------ GR history
class HistoryView : public juce::Component, private juce::Timer {
public:
    explicit HistoryView(Engine& e) : engine(e)
    {
        hist.assign(kN, 0.0f);
        startTimerHz(kRate);
    }

    void paint(juce::Graphics& g) override
    {
        using namespace look;
        auto b = getLocalBounds().toFloat();
        g.setColour(window);
        g.fillRoundedRectangle(b, 8);
        const float s = b.getWidth() / 160.0f;
        auto r = b.reduced(12 * s);
        g.setFont(pf::look::panel(14 * s));
        g.setColour(dim);
        g.drawText("REDUCTION", r.removeFromTop(18 * s), juce::Justification::centredLeft);
        r.removeFromTop(4 * s);
        const float span = 20;
        g.setFont(pf::look::value(11 * s));
        for (int db : { 0, 5, 10, 15, 20 }) {
            const float y = r.getY() + r.getHeight() * (float) db / span;
            g.setColour(dim.withAlpha(0.2f));
            g.drawHorizontalLine((int) y, r.getX(), r.getRight());
            g.setColour(dim.withAlpha(0.8f));
            g.drawText(juce::String(db), juce::Rectangle<float>(r.getRight() - 22 * s, y - 12 * s, 22 * s, 12 * s),
                       juce::Justification::bottomRight);
        }
        juce::Path fill, line;
        fill.startNewSubPath(r.getX(), r.getY());
        for (int i = 0; i < kN; ++i) {
            const float v = hist[(size_t) ((head + i) % kN)];
            const float x = r.getX() + r.getWidth() * (float) i / (kN - 1);
            const float y = r.getY() + r.getHeight() * juce::jlimit(0.0f, span, v) / span;
            fill.lineTo(x, y);
            if (i == 0) line.startNewSubPath(x, y); else line.lineTo(x, y);
        }
        fill.lineTo(r.getRight(), r.getY());
        fill.closeSubPath();
        g.setColour(amber.withAlpha(0.18f));
        g.fillPath(fill);
        g.setColour(amber);
        g.strokePath(line, juce::PathStrokeType(1.6f * s));
        g.setColour(dim);
        g.drawText(juce::String(kN / kRate) + " s", r, juce::Justification::bottomLeft);
    }

private:
    static constexpr int kRate = 30, kN = kRate * 5;
    Engine& engine;
    std::vector<float> hist;
    int head = 0;

    void timerCallback() override
    {
        float v = engine.gainReduction.load();
        hist[(size_t) head] = std::isfinite(v) ? juce::jmax(0.0f, v) : 0.0f;
        head = (head + 1) % kN;
        repaint();
    }
};

} // namespace opto
