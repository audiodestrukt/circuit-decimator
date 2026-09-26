// GainReductionView -- a compressor's gain reduction: a VU-style needle
// (0 to -40 dB, like the LA-2A's meter in GR mode) and a scrolling history of
// the last few seconds, so attack, release and the T4's "memory" are visible.
// The audio thread writes one atomic (dB); the view samples it on a timer.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <atomic>
#include <cmath>
#include <vector>

namespace cd::ui {

class GainReductionView : public juce::Component, private juce::Timer {
public:
    explicit GainReductionView(const std::atomic<float>& grDb) : gr(grDb)
    {
        history.assign(kHistory, 0.0f);
        startTimerHz(kRate);
    }

    void clear()
    {
        std::fill(history.begin(), history.end(), 0.0f);
        needle = 0;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto bg = juce::Colour(0xff1b1c1e), dim = juce::Colour(0xff8a8f98), fg = juce::Colour(0xffc9ccd1);
        const auto face = juce::Colour(0xfff1e3c2), ink = juce::Colour(0xff2a2622), red = juce::Colour(0xffc2412f);
        const auto trace = juce::Colour(0xfff2b27a);
        g.fillAll(bg);
        auto r = getLocalBounds().reduced(10, 8);
        g.setColour(dim);
        g.setFont(juce::FontOptions(13.0f));
        auto head = r.removeFromTop(20);
        g.drawText("Gain reduction", head, juce::Justification::centredLeft);
        g.setColour(fg);
        g.drawText(juce::String(needle, 1) + " dB", head, juce::Justification::centredRight);
        r.removeFromTop(4);

        // ---- VU face ---------------------------------------------------------
        auto meterArea = r.removeFromTop(juce::jmin(r.getHeight() / 2, (int) (r.getWidth() * 0.45f))).toFloat();
        g.setColour(face);
        g.fillRoundedRectangle(meterArea, 6);
        const float cx = meterArea.getCentreX(), cy = meterArea.getBottom() + meterArea.getHeight() * 0.35f;
        const float rad = meterArea.getHeight() * 1.05f;
        // 0 dB at the right end of the arc (no reduction), -40 at the left
        auto angleFor = [](float db) { return juce::jmap(juce::jlimit(0.0f, 40.0f, db), 0.0f, 40.0f, 0.75f, -0.75f); };
        g.setFont(juce::FontOptions(11.0f));
        for (int db : { 0, 1, 2, 3, 5, 7, 10, 15, 20, 30, 40 }) {
            const float a = angleFor(scaleDb((float) db));
            const float s = std::sin(a), c = std::cos(a);
            g.setColour(db >= 20 ? red : ink);
            g.drawLine(cx + s * rad * 0.86f, cy - c * rad * 0.86f, cx + s * rad * 0.95f, cy - c * rad * 0.95f, 1.5f);
            g.drawText(juce::String(db), juce::Rectangle<float>(cx + s * rad * 1.02f - 12, cy - c * rad * 1.02f - 8, 24, 14),
                       juce::Justification::centred);
        }
        {
            const float a = angleFor(scaleDb(needle));
            juce::Graphics::ScopedSaveState save(g);
            g.reduceClipRegion(meterArea.toNearestInt());
            g.setColour(ink);
            g.drawLine(cx, cy, cx + std::sin(a) * rad * 0.97f, cy - std::cos(a) * rad * 0.97f, 1.8f);
        }
        g.setColour(ink.withAlpha(0.6f));
        g.drawText("dB GR", meterArea.withTrimmedTop(meterArea.getHeight() * 0.6f), juce::Justification::centred);

        // ---- history ------------------------------------------------------------
        r.removeFromTop(8);
        auto plot = r.toFloat();
        g.setColour(juce::Colour(0xff26282c));
        g.fillRect(plot);
        const float span = 30;   // dB shown
        g.setFont(juce::FontOptions(11.0f));
        for (int db : { 0, 10, 20, 30 }) {
            const float y = plot.getY() + plot.getHeight() * (float) db / span;
            g.setColour(dim.withAlpha(0.25f));
            g.drawHorizontalLine((int) y, plot.getX(), plot.getRight());
            g.setColour(dim);
            g.drawText(juce::String(db), juce::Rectangle<float>(plot.getX() + 2, y, 24, 12), juce::Justification::topLeft);
        }
        g.setColour(dim);
        g.drawText(juce::String(kHistory / kRate) + " s", plot.reduced(4), juce::Justification::bottomLeft);
        juce::Path p;
        for (int i = 0; i < kHistory; ++i) {
            const float v = history[(size_t) ((head0 + i) % kHistory)];
            const float x = plot.getX() + plot.getWidth() * (float) i / (kHistory - 1);
            const float y = plot.getY() + plot.getHeight() * juce::jlimit(0.0f, span, v) / span;
            if (i == 0) p.startNewSubPath(x, y);
            else p.lineTo(x, y);
        }
        g.setColour(trace);
        g.strokePath(p, juce::PathStrokeType(1.6f));
    }

private:
    static constexpr int kRate = 30, kHistory = kRate * 6;
    const std::atomic<float>& gr;
    std::vector<float> history;
    int head0 = 0;
    float needle = 0;

    // VU-ish spacing: stretch the first few dB like a real meter scale
    static float scaleDb(float db) { return 40.0f * std::sqrt(juce::jlimit(0.0f, 40.0f, db) / 40.0f); }

    void timerCallback() override
    {
        float v = gr.load(std::memory_order_relaxed);
        if (!std::isfinite(v)) v = 0;
        v = juce::jmax(0.0f, v);
        needle += (v - needle) * 0.35f;   // meter ballistics
        history[(size_t) head0] = v;
        head0 = (head0 + 1) % kHistory;
        repaint();
    }
};

} // namespace cd::ui
