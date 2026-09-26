// NodeMeterView -- a multimeter for a netlist circuit: each probe node's DC
// voltage and how much signal swings on it (bar on a dB scale). The audio
// thread writes ProbeTelemetry; the view reads it on a timer.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

namespace cd::ui {

struct ProbeTelemetry {
    static constexpr int kMax = 12;
    std::array<std::atomic<float>, kMax> mean {}, swing {};
};

class NodeMeterView : public juce::Component, private juce::Timer {
public:
    explicit NodeMeterView(const ProbeTelemetry& t) : tele(t) { startTimerHz(20); }

    void setProbes(std::vector<std::string> labels)
    {
        names = std::move(labels);
        shownMean.fill(0);
        shownSwing.fill(0);
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const auto bg = juce::Colour(0xff1b1c1e), dim = juce::Colour(0xff8a8f98), fg = juce::Colour(0xffc9ccd1);
        const auto glow = juce::Colour(0xff7fd18b);
        g.fillAll(bg);
        auto r = getLocalBounds().reduced(10, 8);
        g.setColour(dim);
        g.setFont(juce::FontOptions(13.0f));
        auto head = r.removeFromTop(20);
        g.drawText("node", head.removeFromLeft(90), juce::Justification::centredLeft);
        g.drawText("DC", head.removeFromLeft(80), juce::Justification::centredRight);
        head.removeFromLeft(12);
        g.drawText("signal swing (peak-peak)", head, juce::Justification::centredLeft);
        const int n = (int) std::min(names.size(), (size_t) ProbeTelemetry::kMax);
        const int rowH = n > 0 ? std::min(26, r.getHeight() / n) : 26;
        for (int k = 0; k < n; ++k) {
            auto row = r.removeFromTop(rowH);
            g.setColour(fg);
            g.setFont(juce::FontOptions(14.0f));
            g.drawText(names[(size_t) k], row.removeFromLeft(90), juce::Justification::centredLeft);
            g.drawText(juce::String(shownMean[(size_t) k], 2) + " V", row.removeFromLeft(80), juce::Justification::centredRight);
            row.removeFromLeft(12);
            const auto txt = row.removeFromRight(80);
            auto bar = row.reduced(0, rowH / 4).toFloat();
            g.setColour(juce::Colour(0xff2a2c30));
            g.fillRect(bar);
            // -80 dBV .. +50 dBV
            const float s = shownSwing[(size_t) k];
            const float db = 20.0f * std::log10(std::max(s, 1e-5f));
            const float f = juce::jlimit(0.0f, 1.0f, (db + 80.0f) / 130.0f);
            g.setColour(glow.withAlpha(0.35f + 0.5f * f));
            g.fillRect(bar.withWidth(bar.getWidth() * f));
            g.setColour(dim);
            g.drawText(s >= 1 ? juce::String(s, 1) + " V" : s >= 1e-3f ? juce::String(s * 1e3f, 1) + " mV"
                                                                         : juce::String(s * 1e6f, 0) + " uV",
                       txt, juce::Justification::centredRight);
        }
    }

private:
    const ProbeTelemetry& tele;
    std::vector<std::string> names;
    std::array<float, ProbeTelemetry::kMax> shownMean {}, shownSwing {};

    void timerCallback() override
    {
        for (size_t k = 0; k < (size_t) ProbeTelemetry::kMax; ++k) {
            const float m = tele.mean[k].load(), s = tele.swing[k].load();
            if (std::isfinite(m)) shownMean[k] += 0.3f * (m - shownMean[k]);
            if (std::isfinite(s)) shownSwing[k] += (s > shownSwing[k] ? 0.5f : 0.1f) * (s - shownSwing[k]);
        }
        repaint();
    }
};

} // namespace cd::ui
