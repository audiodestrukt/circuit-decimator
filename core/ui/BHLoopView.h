// BHLoopView -- a transformer core's live B-H loop. The audio thread pushes
// (B, H) samples into a BHTrace; the view zooms onto the recent loop (so its
// shape is always readable) and shows a full-range inset with where the loop
// sits relative to zero and saturation. Shared by Iron and Circuit Bench.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <atomic>
#include <functional>

namespace cd::ui {

// lock-free ring of recent (B, H) points: one writer (audio), any readers (UI)
struct BHTrace {
    static constexpr int kSize = 2048;
    std::array<std::atomic<float>, kSize> b {}, h {};
    std::atomic<int> write { 0 };
    void push(float bv, float hv)
    {
        const int w = write.load(std::memory_order_relaxed);
        b[(size_t) (w & (kSize - 1))] = bv;
        h[(size_t) (w & (kSize - 1))] = hv;
        write.store(w + 1, std::memory_order_release);
    }
};

namespace bhlook {
inline const juce::Colour plot { 0xff1e2226 };
inline const juce::Colour grid { 0xff3a4048 };
inline const juce::Colour copper { 0xffc8703a };
inline const juce::Colour hot { 0xfff2b27a };
inline const juce::Colour silk { 0xffe6e2da };
inline const juce::Colour ember { 0xffff8a3d };
} // namespace bhlook

class BHLoopView : public juce::Component, private juce::Timer {
public:
    explicit BHLoopView(const BHTrace& t) : trace(t) { startTimerHz(30); }

    // fonts for labels (default: the JUCE default face)
    std::function<juce::Font(float)> valueFont = [](float size) { return juce::Font(juce::FontOptions(size)); };
    std::function<juce::Font(float)> panelFont = [](float size) { return juce::Font(juce::FontOptions(size)); };

    void paint(juce::Graphics& g) override
    {
        using namespace bhlook;
        g.fillAll(plot);
        // plot below a header strip that holds the title and the full-range inset
        const auto r = getLocalBounds().toFloat().reduced(46, 28).withTrimmedBottom(10).withTrimmedTop(70);
        auto px = [&](float hh, float bb) {
            return juce::Point<float>(r.getX() + (hh - hLo) / (hHi - hLo) * r.getWidth(),
                                      r.getBottom() - (bb - bLo) / (bHi - bLo) * r.getHeight());
        };
        // saturation zones, if the zoom reaches them
        g.setColour(ember.withAlpha(0.08f));
        if (bHi > 1.2f) g.fillRect(juce::Rectangle<float>::leftTopRightBottom(r.getX(), r.getY(), r.getRight(), px(0, 1.2f).y));
        if (bLo < -1.2f) g.fillRect(juce::Rectangle<float>::leftTopRightBottom(r.getX(), px(0, -1.2f).y, r.getRight(), r.getBottom()));

        // grid with round-number ticks for the current zoom
        auto ticks = [](float lo, float hi) {
            const float span = juce::jmax(hi - lo, 1e-6f), raw = span / 4, mag = std::pow(10.0f, std::floor(std::log10(raw)));
            const float step = raw / mag < 2 ? 2 * mag : raw / mag < 5 ? 5 * mag : 10 * mag;
            return std::pair<float, float>(std::ceil(lo / step) * step, step);
        };
        g.setFont(valueFont(13));
        // count-bounded: a float `v += step` loop can stall forever if step is
        // tiny relative to v (e.g. a degenerate zoom)
        auto [b0, bs] = ticks(bLo, bHi);
        for (int t = 0; t < 12; ++t) {
            const float bl = b0 + (float) t * bs;
            if (!(bl <= bHi)) break;
            g.setColour(grid);
            g.drawHorizontalLine((int) px(0, bl).y, r.getX(), r.getRight());
            g.setColour(silk.withAlpha(0.55f));
            // millitesla once zoomed in that far, so small loops keep readable labels
            const bool milli = bHi - bLo < 0.1f;
            const float shown = milli ? bl * 1000.0f : bl, step = milli ? bs * 1000.0f : bs;
            g.drawText(juce::String(shown, step < 0.1f ? 2 : step < 1 ? 1 : 0) + (milli ? " mT" : " T"), juce::Rectangle<float>(r.getX() - 46, px(0, bl).y - 8, 42, 16),
                       juce::Justification::centredRight);
        }
        auto [h0, hs] = ticks(hLo, hHi);
        for (int t = 0; t < 12; ++t) {
            const float hl = h0 + (float) t * hs;
            if (!(hl <= hHi)) break;
            g.setColour(grid);
            g.drawVerticalLine((int) px(hl, 0).x, r.getY(), r.getBottom());
            g.setColour(silk.withAlpha(0.55f));
            g.drawText(hs < 1 ? juce::String(hl, hs < 0.1f ? 2 : 1) : juce::String(juce::roundToInt(hl)), juce::Rectangle<float>(px(hl, 0).x - 20, r.getBottom() + 4, 40, 16),
                       juce::Justification::centred);
        }
        g.drawText("A/m", juce::Rectangle<float>(r.getRight() - 40, r.getBottom() + 18, 40, 14), juce::Justification::centredRight);
        g.setFont(panelFont(12));
        g.drawText("B-H loop", juce::Rectangle<float>(r.getX(), r.getY() - 88, 120, 18), juce::Justification::centredLeft);
        g.setFont(valueFont(13));
        g.setColour(silk.withAlpha(0.55f));
        g.drawText("zoomed on the loop", juce::Rectangle<float>(r.getX(), r.getY() - 70, 140, 16), juce::Justification::centredLeft);

        // the trace, older points fading
        g.saveState();
        g.reduceClipRegion(r.toNearestInt());
        for (int i = 1; i < pts; ++i) {
            const float a = (float) i / (float) pts;
            g.setColour(hot.withAlpha(0.08f + 0.85f * a * a));
            g.drawLine({ px(h[(size_t) i - 1], b[(size_t) i - 1]), px(h[(size_t) i], b[(size_t) i]) }, 1.0f + 1.4f * a);
        }
        const auto rest = px(hMean, bMean);
        g.setColour(copper);
        g.fillEllipse(rest.x - 4, rest.y - 4, 8, 8);
        g.restoreState();

        // inset: the whole curve, showing where this loop sits (DC bias, saturation)
        const auto in = juce::Rectangle<float>(r.getRight() - 104, r.getY() - 92, 104, 66);
        g.setColour(plot.darker(0.4f).withAlpha(0.92f));
        g.fillRoundedRectangle(in, 4);
        g.setColour(grid.brighter(0.2f));
        g.drawRoundedRectangle(in, 4, 1);
        const float fullH = juce::jmax(200.0f, 1.3f * juce::jmax(std::abs(hLo), std::abs(hHi)));
        auto ip = [&](float hh, float bb) {
            return juce::Point<float>(in.getCentreX() + hh / fullH * in.getWidth() / 2, in.getCentreY() - bb / 1.6f * in.getHeight() / 2);
        };
        g.setColour(ember.withAlpha(0.15f));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(in.getX(), in.getY(), in.getRight(), ip(0, 1.2f).y));
        g.fillRect(juce::Rectangle<float>::leftTopRightBottom(in.getX(), ip(0, -1.2f).y, in.getRight(), in.getBottom()));
        g.setColour(grid.brighter(0.4f));
        g.drawLine({ ip(-fullH, 0), ip(fullH, 0) }, 0.8f);
        g.drawLine({ ip(0, -1.6f), ip(0, 1.6f) }, 0.8f);
        const auto box = juce::Rectangle<float>::leftTopRightBottom(ip(hLo, 0).x, ip(0, bHi).y, ip(hHi, 0).x, ip(0, bLo).y);
        g.setColour(hot);
        g.drawRect(box.expanded(1.5f), 1.2f);
        g.setColour(silk.withAlpha(0.5f));
        g.setFont(valueFont(11));
        g.drawText("full range", in.reduced(4, 2), juce::Justification::bottomLeft);
    }

private:
    const BHTrace& trace;
    static constexpr int kPts = 1024;
    std::array<float, kPts> b {}, h {};
    int pts = 0;
    float hLo = -80, hHi = -40, bLo = -0.6f, bHi = -0.2f, bMean = 0, hMean = 0;

    void timerCallback() override
    {
        const int w = trace.write.load(std::memory_order_acquire);
        pts = juce::jmin(kPts, w);
        float lo = 1e9f, hi = -1e9f, blo = 1e9f, bhi = -1e9f, sb = 0, sh = 0;
        for (int i = 0; i < pts; ++i) {
            const int k = (w - pts + i) & (BHTrace::kSize - 1);
            const float bv = trace.b[(size_t) k].load(), hv = trace.h[(size_t) k].load();
            // a blown-up solver sample must not poison the zoom
            b[(size_t) i] = std::isfinite(bv) ? juce::jlimit(-5.0f, 5.0f, bv) : 0.0f;
            h[(size_t) i] = std::isfinite(hv) ? juce::jlimit(-1e5f, 1e5f, hv) : 0.0f;
            lo = juce::jmin(lo, h[(size_t) i]);
            hi = juce::jmax(hi, h[(size_t) i]);
            blo = juce::jmin(blo, b[(size_t) i]);
            bhi = juce::jmax(bhi, b[(size_t) i]);
            sb += b[(size_t) i];
            sh += h[(size_t) i];
        }
        if (pts > 0 && std::isfinite(sb) && std::isfinite(sh)) {
            bMean = sb / (float) pts;
            hMean = sh / (float) pts;
            // zoom follows the loop (with margins and a minimum span), smoothed so it doesn't jitter
            auto follow = [](float& vlo, float& vhi, float dlo, float dhi, float minSpan) {
                const float c = 0.5f * (dlo + dhi), span = juce::jmax(minSpan, 1.4f * (dhi - dlo));
                vlo += 0.15f * ((c - span / 2) - vlo);
                vhi += 0.15f * ((c + span / 2) - vhi);
            };
            // small minimum spans: a mic transformer at mic level swings only
            // milliteslas, and its loop should still be readable
            follow(hLo, hHi, lo, hi, 0.1f);
            follow(bLo, bHi, blo, bhi, 0.001f);
        }
        repaint();
    }
};

} // namespace cd::ui
