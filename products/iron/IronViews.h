// Iron's two live views: the stage schematic, with the output transformer
// drawn as its physical core so the knobs visibly change it, and the core's
// B-H loop. Both read the engine's telemetry on a timer.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "IronEngine.h"
#include "ui/BHLoopView.h"
#include "phys-fuzz/PhysFuzzLook.h"

namespace iron::look {

inline const juce::Colour panel { 0xff2a2f35 };
inline const juce::Colour plot { 0xff1e2226 };
inline const juce::Colour grid { 0xff3a4048 };
inline const juce::Colour steel { 0xffa9bccf };
inline const juce::Colour rust { 0xff8a4b2a };
inline const juce::Colour copper { 0xffc8703a };
inline const juce::Colour hot { 0xfff2b27a };
inline const juce::Colour silk { 0xffe6e2da };
inline const juce::Colour ember { 0xffff8a3d };

inline juce::Colour steelColour(float scrap) { return steel.interpolatedWith(rust, scrap); }

// lamination texture: fine horizontal lines over a base colour
inline void laminate(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour base, float pitch)
{
    g.setColour(base);
    g.fillRect(r);
    g.setColour(juce::Colours::black.withAlpha(0.22f));
    for (float y = r.getY() + pitch; y < r.getBottom(); y += pitch) g.drawHorizontalLine((int) y, r.getX(), r.getRight());
}

} // namespace iron::look

namespace iron {

// ---------------------------------------------------------------- schematic
class SchematicView : public juce::Component, private juce::Timer {
public:
    explicit SchematicView(Engine& e) : engine(e) { startTimerHz(30); }

    void paint(juce::Graphics& g) override
    {
        using namespace look;
        const float W = 560, H = 380;
        const float s = juce::jmin((float) getWidth() / W, (float) getHeight() / H);
        g.fillAll(plot);
        g.addTransform(juce::AffineTransform::scale(s).translated(((float) getWidth() - W * s) / 2,
                                                                   ((float) getHeight() - H * s) / 2));
        const float power = juce::jlimit(0.0f, 1.0f, supply / 250.0f);
        auto label = [&](const juce::String& t, float x, float y, float size, juce::Colour c,
                         juce::Justification j = juce::Justification::centredLeft) {
            g.setColour(c);
            g.setFont(pf::look::value(size));
            const float w = 160;
            const float x0 = j.testFlags(juce::Justification::right) ? x - w : j.testFlags(juce::Justification::horizontallyCentred) ? x - w / 2 : x;
            g.drawText(t, juce::Rectangle<float>(x0, y - size, w, size * 2), j, false);
        };
        auto wire = [&](std::initializer_list<juce::Point<float>> pts, int probe) {
            juce::Path p;
            bool first = true;
            for (auto q : pts) { if (first) p.startNewSubPath(q); else p.lineTo(q); first = false; }
            const float sw = probe >= 0 ? swing[(size_t) probe] : 0.0f;
            if (sw > 0.02f) {
                g.setColour(hot.withAlpha(0.12f + 0.3f * sw));
                g.strokePath(p, juce::PathStrokeType(4 + 12 * sw, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            g.setColour(copper.interpolatedWith(hot, sw).withMultipliedAlpha(0.35f + 0.65f * power));
            g.strokePath(p, juce::PathStrokeType(2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        };
        auto ground = [&](float x, float y) {
            g.setColour(silk.withAlpha(0.55f));
            g.drawLine(x, y, x, y + 8, 1.6f);
            for (int i = 0; i < 3; ++i) g.drawLine(x - 11 + 4 * (float) i, y + 8 + 4 * (float) i, x + 11 - 4 * (float) i, y + 8 + 4 * (float) i, 1.6f);
        };
        auto resistor = [&](float x, float y0, float y1) {   // vertical zigzag
            juce::Path p;
            p.startNewSubPath(x, y0);
            const float body = (y1 - y0) * 0.6f, st = y0 + (y1 - y0 - body) / 2;
            p.lineTo(x, st);
            for (int i = 0; i < 6; ++i) p.lineTo(x + ((i % 2) ? -6.0f : 6.0f), st + body * ((float) i + 0.5f) / 6);
            p.lineTo(x, st + body);
            p.lineTo(x, y1);
            g.setColour(silk.withAlpha(0.8f));
            g.strokePath(p, juce::PathStrokeType(1.6f));
        };

        // ---- supply rail
        wire({ { 150, 42 }, { 350, 42 } }, Engine::Supply);
        label("B+ " + juce::String(supply, 0) + " V", 150, 26, 15, power < 0.98f ? ember : silk.withAlpha(0.8f));

        // ---- input, grid leak
        g.setColour(silk.withAlpha(0.8f));
        g.drawEllipse(20, 214, 14, 14, 1.8f);
        label("in", 18, 246, 15, silk.withAlpha(0.7f));
        wire({ { 34, 221 }, { 128, 221 } }, Engine::Grid);
        resistor(84, 221, 290);
        ground(84, 290);

        // ---- 12AU7: envelope, heater glow, cathode, grid, plate
        const juce::Point<float> tc(170, 205);
        g.setColour(ember.withAlpha(0.12f + 0.25f * power));
        g.fillEllipse(tc.x - 30, tc.y + 18, 60, 30);          // heater glow
        g.setColour(silk.withAlpha(0.85f));
        g.drawEllipse(tc.x - 46, tc.y - 56, 92, 112, 1.8f);
        g.drawLine(tc.x - 24, tc.y - 30, tc.x + 24, tc.y - 30, 3.0f);   // plate
        const float dashes[] = { 5, 4 };
        g.drawDashedLine({ tc.x - 42, tc.y + 16, tc.x + 30, tc.y + 16 }, dashes, 2, 1.6f);   // grid
        g.drawLine(tc.x - 22, tc.y + 34, tc.x + 22, tc.y + 34, 2.4f);                         // cathode
        g.setColour(ember.withAlpha(0.3f + 0.6f * power));
        juce::Path heater;
        heater.startNewSubPath(tc.x - 14, tc.y + 46);
        heater.quadraticTo(tc.x, tc.y + 34, tc.x + 14, tc.y + 46);
        g.strokePath(heater, juce::PathStrokeType(2.2f));
        wire({ { 128, 221 }, { tc.x - 42, tc.y + 16 } }, Engine::Grid);
        label("12AU7", tc.x + 52, tc.y - 44, 15, silk.withAlpha(0.8f));

        // cathode: Rk || Ck to ground
        wire({ { tc.x, tc.y + 34 }, { tc.x, 290 } }, Engine::Cathode);
        wire({ { tc.x, 262 }, { tc.x + 34, 262 } }, Engine::Cathode);
        resistor(tc.x, 262, 300);
        g.setColour(silk.withAlpha(0.8f));
        g.drawLine(tc.x + 34, 262, tc.x + 34, 278, 1.6f);
        g.drawLine(tc.x + 22, 278, tc.x + 46, 278, 2.4f);
        g.drawLine(tc.x + 22, 285, tc.x + 46, 285, 2.4f);
        g.drawLine(tc.x + 34, 285, tc.x + 34, 300, 1.6f);
        ground(tc.x, 300);
        ground(tc.x + 34, 300);

        // ---- the output transformer, drawn as its core
        const float core = juce::jlimit(0.5f, 2.0f, knob[kCore] / 100.0f);
        const float t = 10 + 12 * (core - 0.5f) / 1.5f;                // leg thickness follows core size
        const auto frame = juce::Rectangle<float>(330, 72, 130, 236);
        const float scrap = knob[kSteel] / 100.0f;
        auto base = steelColour(scrap);
        // flux brightens the steel: you can see the DC bias the gap sets
        const float fluxGlow = juce::jlimit(0.0f, 1.0f, std::abs(flux) / 1.4f);
        base = base.interpolatedWith(hot, 0.45f * fluxGlow);
        auto legs = { frame.withWidth(t), frame.withLeft(frame.getRight() - t),
                      frame.withHeight(t), frame.withTop(frame.getBottom() - t),
                      frame.withSizeKeepingCentre(t, frame.getHeight()) };   // E-I: outer legs + centre leg
        for (auto r : legs) laminate(g, r, base, 3.0f);
        if (scrap > 0.4f) {   // rust bloom on scrap steel
            juce::Random rng(7);
            g.setColour(rust.darker(0.3f).withAlpha(0.6f * (scrap - 0.4f) / 0.6f));
            for (int i = 0; i < 90; ++i) {
                const auto r = *(legs.begin() + rng.nextInt(5));
                g.fillEllipse(r.getX() + rng.nextFloat() * r.getWidth(), r.getY() + rng.nextFloat() * r.getHeight(), 3, 2);
            }
        }
        // the air gap: a slit across all three legs between the E and the I
        const float gapPx = 1.5f + 16.0f * (float) std::log(knob[kGap] / 0.03) / (float) std::log(10.0);
        const float gy = frame.getBottom() - t - gapPx - 2;
        g.setColour(plot);
        g.fillRect(frame.getX() - 1, gy, frame.getWidth() + 2, gapPx);
        label(juce::String(knob[kGap], 2) + " mm gap", frame.getX() - 8, gy + gapPx / 2, 14, silk.withAlpha(0.75f),
              juce::Justification::right);
        label(juce::String(flux, 2) + " T", frame.getCentreX(), frame.getY() - 12, 15,
              hot.withAlpha(0.6f + 0.4f * fluxGlow), juce::Justification::centred);

        // windings on the centre leg: primary (plate + B+), secondary (out)
        auto coil = [&](float x0, float y0, float y1, int probe) {
            const float sw = swing[(size_t) probe];
            for (int turn = 0; turn < 30; ++turn) {
                const float y = y0 + 7.0f * (float) turn;
                if (!(y < y1)) break;
                g.setColour(copper.interpolatedWith(hot, sw).withMultipliedAlpha(0.5f + 0.5f * power));
                g.drawEllipse(x0, y, 34, 7, 2.2f);
            }
        };
        const float cx = frame.getCentreX() - 17;
        coil(cx, 96, 178, Engine::Primary);
        coil(cx, 200, gy - 6, Engine::Out);
        wire({ { 350, 42 }, { cx + 17, 42 }, { cx + 17, 96 } }, Engine::Supply);
        wire({ { tc.x, tc.y - 30 }, { tc.x, 140 }, { 300, 140 }, { 300, 176 }, { cx, 176 } }, Engine::Plate);
        wire({ { cx + 34, 204 }, { 500, 204 }, { 500, 221 }, { 520, 221 } }, Engine::Out);
        wire({ { cx + 34, gy - 8 }, { 486, gy - 8 }, { 486, 300 } }, -1);
        ground(486, 300);
        g.setColour(silk.withAlpha(0.8f));
        g.drawEllipse(520, 214, 14, 14, 1.8f);
        label("out", 516, 246, 15, silk.withAlpha(0.7f));
        label(scrap < 0.15f ? "premium steel" : scrap < 0.6f ? "silicon steel" : "scrap steel", frame.getCentreX(),
              frame.getBottom() + 18, 14, steelColour(scrap).brighter(0.2f), juce::Justification::centred);
    }

private:
    Engine& engine;
    float knob[kNumKnobs] { 0, 0.1f, 100, 30, 0, 100 };   // until the first timer tick
    std::array<float, Engine::kProbes> swing {};
    float flux = 0, supply = 250;

    void timerCallback() override
    {
        // clamp to the knob ranges: geometry below takes logs and divides by these
        for (int i = 0; i < kNumKnobs; ++i)
            knob[i] = juce::jlimit((float) kKnobs[i].min, (float) kKnobs[i].max, engine.knobsInUse[(size_t) i].load());
        // swings normalised per probe to a sensible visual range, smoothed
        const float scale[Engine::kProbes] = { 1.0f, 0.3f, 20.0f, 5.0f, 20.0f, 5.0f };
        for (int k = 0; k < Engine::kProbes; ++k) {
            const float s = juce::jlimit(0.0f, 1.0f, engine.probeSwing[(size_t) k].load() / scale[k]);
            swing[(size_t) k] += (s > swing[(size_t) k] ? 0.5f : 0.1f) * (s - swing[(size_t) k]);
        }
        flux += 0.3f * (engine.fluxMean.load() - flux);
        supply = engine.supplyVolts.load();
        repaint();
    }
};

} // namespace iron
