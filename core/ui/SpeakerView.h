// SpeakerView -- a live cross-section of the speaker cab, drawn to scale from the
// same parameters the simulation runs on (acoustic/CabModel.h), with the mic's
// response underneath.
//
//   section:  box and baffle (depth from the volume), magnet (size from Bl),
//             voice coil, spider, basket; the cone's real profile (depth, curve)
//             with its paper thickness exaggerated (taper, ribs as corrugation,
//             colour from density); surround; dust cap; the mic at its position
//             and angle, capsule to scale, a ghost of its polar pattern.
//   motion:   the cone's actual deflection shape at the cursor frequency (from
//             the shell model), animated and exaggerated, coloured by how much
//             each part moves relative to the voice coil.
//   response: dB SPL at the mic for 2.83 V; drag across it to move the cursor.
//
// Reads Cab::paramsSnapshot() and Cab::display() on a timer (UI thread only).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "acoustic/CabModel.h"

#include <functional>

namespace cd::ui {

class SpeakerView : public juce::Component, private juce::Timer {
public:
    // source: returns the cab to show (or nullptr); called on the UI thread
    explicit SpeakerView(std::function<const acoustic::Cab*()> source) : cabSource(std::move(source)) { startTimerHz(30); }

    void paint(juce::Graphics& g) override
    {
        using namespace acoustic;
        g.fillAll(bg);
        const auto* cab = cabSource ? cabSource() : nullptr;
        if (!cab || disp.nodeR.empty()) {
            g.setColour(dim);
            g.drawText("no speaker", getLocalBounds(), juce::Justification::centred);
            return;
        }
        auto area = getLocalBounds().toFloat().reduced(10);
        auto plot = area.removeFromBottom(juce::jmax(90.0f, area.getHeight() * 0.3f));
        area.removeFromBottom(8);
        drawSection(g, area);
        drawResponse(g, plot);
    }

    void setCursor(double hz) { cursorHz = hz; repaint(); }
    void mouseDown(const juce::MouseEvent& e) override { setCursorFrom(e.position); }
    void mouseDrag(const juce::MouseEvent& e) override { setCursorFrom(e.position); }

private:
    std::function<const acoustic::Cab*()> cabSource;
    acoustic::CabParams p {};
    acoustic::CabDisplay disp;
    unsigned seenVersion = ~0u;
    double cursorHz = 2300, phase = 0;
    juce::Rectangle<float> plotArea;

    const juce::Colour bg { 0xff1b1c1e }, dim { 0xff8a8f98 }, fg { 0xffc9ccd1 }, accent { 0xfff2b27a };
    const juce::Colour steel { 0xff6f7780 }, magnet { 0xff3a3f46 }, copper { 0xffc8703a }, wood { 0xff4a3b2c };

    void timerCallback() override
    {
        const auto* cab = cabSource ? cabSource() : nullptr;
        if (!cab) return;
        p = cab->paramsSnapshot();
        if (cab->displayVersion() != seenVersion) {
            seenVersion = cab->displayVersion();
            disp = cab->display();
        }
        phase += 2 * M_PI / 30 * 0.8;   // the animation runs at 0.8 Hz, whatever the frequency shown
        repaint();
    }

    void setCursorFrom(juce::Point<float> pt)
    {
        if (!plotArea.contains(pt) || disp.freq.empty()) return;
        const double t = (pt.x - plotArea.getX()) / plotArea.getWidth();
        cursorHz = disp.freq.front() * std::pow(disp.freq.back() / disp.freq.front(), juce::jlimit(0.0, 1.0, t));
        repaint();
    }

    // nearest precomputed deflection shape to the cursor
    const std::vector<std::complex<double>>* shapeAt(double f, double& fShown) const
    {
        if (disp.shapeFreq.empty()) return nullptr;
        size_t best = 0;
        for (size_t i = 1; i < disp.shapeFreq.size(); ++i)
            if (std::abs(std::log(disp.shapeFreq[i] / f)) < std::abs(std::log(disp.shapeFreq[best] / f))) best = i;
        fShown = disp.shapeFreq[best];
        return &disp.shape[best];
    }

    // ------------------------------------------------------------------ the section
    void drawSection(juce::Graphics& g, juce::Rectangle<float> area)
    {
        using namespace acoustic;
        const double a = p[kConeRadius], rc = p[kCoilRadius], depth = p[kDepth];
        const double boxFront = 0.45, boxDepth = p[kVb] / (boxFront * boxFront);
        const double z0 = -depth;                                   // the neck
        const double motor = 0.012 * juce::jlimit(0.4, 2.5, p[kBl] / 10.9);   // magnet height from Bl
        const double zBack = z0 - 0.03 - motor - 0.01;
        // frame the speaker and the mic; the box runs off the edges
        const double zMin = zBack - 0.02;
        const double zMax = std::min(p[kMicDistance], 0.3) + 0.14;
        const double rMax = a + 0.05;
        const float sc = (float) juce::jmin(area.getWidth() / (zMax - zMin), area.getHeight() / (2 * rMax));
        const float cx = area.getCentreX() - (float) ((zMax + zMin) / 2) * sc, cy = area.getCentreY();
        auto P = [&](double z, double r) { return juce::Point<float>(cx + (float) z * sc, cy - (float) r * sc); };
        juce::Graphics::ScopedSaveState clipSection(g);
        g.reduceClipRegion(area.toNearestInt());

        // ---- box, stuffing, baffle
        const auto boxR = juce::Rectangle<float>(P(-boxDepth, boxFront / 2), P(0, -boxFront / 2));
        g.setColour(wood.withAlpha(0.25f));
        g.fillRect(boxR);
        g.setColour(wood.brighter(0.4f));
        g.drawRect(boxR, 3.0f);
        {
            // absorption: more stuffing (hatching) for a lower Qa
            juce::Graphics::ScopedSaveState clipBox(g);
            g.reduceClipRegion(boxR.toNearestInt());
            const int hatch = juce::jlimit(0, 60, (int) (240 / juce::jmax(2.0, p[kQa])));
            g.setColour(dim.withAlpha(0.18f));
            for (int i = 0; i < hatch; ++i) {
                const float x = boxR.getX() + boxR.getWidth() * ((float) i + 0.5f) / (float) hatch;
                g.drawLine(x, boxR.getY(), x - boxR.getHeight() * 0.3f, boxR.getBottom(), 1.0f);
            }
        }
        g.setColour(wood.brighter(0.6f));
        for (int s = -1; s <= 1; s += 2) {
            const auto b = juce::Rectangle<float>(P(-0.018, s > 0 ? boxFront / 2 : -(a + 0.01)), P(0, s > 0 ? a + 0.01 : -boxFront / 2));
            g.fillRect(b);
        }
        {
            juce::String bl = "closed box " + juce::String(juce::roundToInt(p[kVb] * 1e3)) + " l (" + juce::String(juce::roundToInt(boxDepth * 100))
                              + " cm deep behind a 45 cm baffle), Qa " + juce::String(juce::roundToInt(p[kQa]));
            if (P(-boxDepth, 0).x < area.getX()) bl << "  (continues off the left)";
            label(g, bl, { area.getX() + 4, area.getBottom() - 34 });
        }

        for (int side = -1; side <= 1; side += 2) {
            auto Q = [&](double z, double r) { return P(z, side * r); };
            // ---- motor: back plate, magnet, top plate, pole
            const double zTop = z0 - 0.018;
            auto rect = [&](double za, double ra, double zb, double rb) { return juce::Rectangle<float>(Q(za, ra), Q(zb, rb)).toFloat(); };
            auto fillR = [&](juce::Rectangle<float> r, juce::Colour c) {
                g.setColour(c);
                g.fillRect(juce::Rectangle<float>::leftTopRightBottom(juce::jmin(r.getX(), r.getRight()), juce::jmin(r.getY(), r.getBottom()),
                                                                       juce::jmax(r.getX(), r.getRight()), juce::jmax(r.getY(), r.getBottom())));
            };
            fillR(rect(zTop - 0.008, rc + 0.0015, zTop, 0.07), steel);                     // top plate
            fillR(rect(zTop - 0.008 - motor, 0.035, zTop - 0.008, 0.075), magnet);          // magnet ring
            fillR(rect(zBack, 0.0, zBack + 0.008, 0.075), steel);                           // back plate
            fillR(rect(zBack + 0.008, 0.0, zTop, rc - 0.0015), steel.darker(0.2f));        // pole piece
            // ---- voice coil former + winding in the gap
            fillR(rect(zTop - 0.012, rc - 0.0008, z0, rc + 0.0008), fg.withAlpha(0.7f));
            fillR(rect(zTop - 0.010, rc - 0.0014, zTop + 0.002, rc + 0.0014), copper);
            // ---- spider
            juce::Path spider;
            const double zs = z0 - 0.009;
            spider.startNewSubPath(Q(zs, rc));
            for (int i = 1; i <= 8; ++i) {
                const double r = rc + (0.068 - rc) * i / 8.0;
                spider.lineTo(Q(zs + ((i % 2) ? 0.003 : -0.003), r));
            }
            g.setColour(juce::Colour(0xffb8a07a));
            g.strokePath(spider, juce::PathStrokeType(1.5f));
            // ---- basket
            g.setColour(steel.withAlpha(0.8f));
            g.drawLine(juce::Line<float>(Q(zTop, 0.07), Q(-0.004, a + 0.016)), 2.0f);
            g.drawLine(juce::Line<float>(Q(zs, 0.07), Q(zTop, 0.07)), 2.0f);
        }

        // ---- cone (rest), thickness exaggerated
        const auto& nr = disp.nodeR;
        const auto& nz = disp.nodeZ;
        const size_t nn = nr.size();
        const double thick = p[kThickness], taper = p[kTaper];
        const float exag = 14.0f;   // thickness drawn x14
        const float dens = (float) juce::jlimit(0.0, 1.0, (p[kDensity] - 250) / 700);
        const auto paper = juce::Colour(0xffd9b98a).interpolatedWith(juce::Colour(0xff6b4a2a), dens);
        for (int side = -1; side <= 1; side += 2) {
            juce::Path cone;
            std::vector<juce::Point<float>> top, bot;
            for (size_t k = 0; k < nn; ++k) {
                const double t = (nr[k] - rc) / std::max(1e-9, a - rc);
                const double h = thick * (taper + (1 - taper) * t) * exag;
                // normal of the profile at k
                const size_t k0 = k == 0 ? 0 : k - 1, k1 = std::min(nn - 1, k + 1);
                double dz = nz[k1] - nz[k0], dr = nr[k1] - nr[k0];
                const double L = std::hypot(dz, dr);
                dz /= L; dr /= L;
                // corrugation: ribs as small bumps along the slope
                double bump = 0;
                if (p[kRibs] > 1.2) {
                    const double nRib = std::round(2 + p[kRibs]);
                    bump = 0.0012 * std::min(1.0, (p[kRibs] - 1) / 6) * std::sin(M_PI * nRib * t) * (t > 0.2 && t < 0.97 ? 1 : 0);
                }
                const double zc = nz[k] + bump * dr, rcn = nr[k] - bump * dz;
                top.push_back(P(zc - dr * h / 2, side * (rcn + dz * h / 2)));
                bot.push_back(P(zc + dr * h / 2, side * (rcn - dz * h / 2)));
            }
            cone.startNewSubPath(top[0]);
            for (auto& q : top) cone.lineTo(q);
            for (size_t k = nn; k-- > 0;) cone.lineTo(bot[k]);
            cone.closeSubPath();
            g.setColour(paper);
            g.fillPath(cone);
            // surround: a half roll at the edge
            juce::Path roll;
            const double rr = 0.0075;
            for (int i = 0; i <= 16; ++i) {
                const double th = M_PI * i / 16.0;
                const auto q = P(rr * std::sin(th) * 0.9, side * (a + rr - rr * std::cos(th)));
                if (i == 0) roll.startNewSubPath(q); else roll.lineTo(q);
            }
            const float damp = (float) juce::jlimit(0.0, 1.0, p[kSurroundR] / 3.0);
            g.setColour(juce::Colour(0xffb89a6a).interpolatedWith(juce::Colour(0xff3d3025), damp));
            g.strokePath(roll, juce::PathStrokeType(2.5f));
        }
        // ---- dust cap: a dome glued at its radius
        {
            const double rd = p[kDustCap], capH = p[kCapHeight];
            const double zg = coneHeight(coneOf(p), rd * 1.0001);
            juce::Path cap;
            for (int i = 0; i <= 40; ++i) {
                const double r = -rd + 2 * rd * i / 40.0;
                const double u = r / rd;
                const auto q = P(zg + capH * (1 - u * u), r);
                if (i == 0) cap.startNewSubPath(q); else cap.lineTo(q);
            }
            const float heavy = (float) juce::jlimit(0.0, 1.0, p[kCapMass] / 2e-3);
            g.setColour(paper.darker(0.3f + 0.6f * heavy));
            g.strokePath(cap, juce::PathStrokeType(2.0f + 2.5f * heavy));
        }

        // ---- the moving cone at the cursor frequency
        double fShown = cursorHz;
        if (const auto* shape = shapeAt(cursorHz, fShown); shape && shape->size() == nn + 1) {
            double mx = 1e-9;
            for (size_t k = 0; k < nn; ++k) mx = std::max(mx, std::abs((*shape)[k]));
            const double amp = 0.025 / mx;   // largest motion drawn as 25 mm
            const auto rot = std::polar(1.0, phase);
            for (int side = -1; side <= 1; side += 2) {
                for (size_t k = 0; k + 1 < nn; ++k) {
                    const auto d0 = ((*shape)[k] * rot).real() * amp, d1 = ((*shape)[k + 1] * rot).real() * amp;
                    const float mag = (float) juce::jlimit(0.0, 1.0, std::abs((*shape)[k]) / mx);
                    g.setColour(juce::Colour(0xff5fb3ff).interpolatedWith(accent, mag).withAlpha(0.95f));
                    g.drawLine(juce::Line<float>(P(nz[k] + d0, side * nr[k]), P(nz[k + 1] + d1, side * nr[k + 1])), 2.2f);
                }
                // the dust cap moving with its glue ring
                const auto dc = ((*shape)[nn] * rot).real() * amp;
                const double rd = p[kDustCap];
                const double zg = coneHeight(coneOf(p), rd * 1.0001);
                g.setColour(accent.withAlpha(0.8f));
                g.drawLine(juce::Line<float>(P(zg + p[kCapHeight] + dc, 0), P(zg + dc, side * rd)), 1.5f);
            }
            label(g, "cone motion at " + hz(fShown) + " (x" + juce::String(juce::roundToInt(amp * 1000)) + " exaggerated; blue = moves less than the coil, orange = the most)",
                  { area.getX() + 4, area.getBottom() - 18 });
        }

        // ---- the mic: capsule to scale, body, polar pattern
        {
            const double off = p[kMicOffset], dist = p[kMicDistance], ang = p[kMicAngle], cap = p[kMicCapsule];
            const bool beyond = dist > zMax - 0.03;
            const double zm = beyond ? zMax - 0.03 : dist;
            const double ax = -std::cos(ang), ar = -std::sin(ang);   // axis, pointing at the speaker (and to the centre)
            const auto c = P(zm, off);
            const float capPx = juce::jmax(3.0f, (float) cap * sc);
            // body: away from the capsule along -axis
            juce::Path body;
            const double bl = 0.11, bw = std::max(cap * 1.2, 0.024) / 2;
            const double pz = -ar, pr = ax;   // perpendicular
            auto B = [&](double s, double w) { return P(zm - ax * s + pz * w, off - ar * s + pr * w); };
            body.startNewSubPath(B(cap / 2, bw));
            body.lineTo(B(bl, bw * 0.8));
            body.lineTo(B(bl, -bw * 0.8));
            body.lineTo(B(cap / 2, -bw));
            body.closeSubPath();
            g.setColour(juce::Colour(0xff2c3036));
            g.fillPath(body);
            g.setColour(steel);
            g.strokePath(body, juce::PathStrokeType(1.2f));
            // grille / capsule
            g.setColour(juce::Colour(0xffa9b3bd));
            g.fillEllipse(c.x - capPx / 2, c.y - capPx / 2, capPx, capPx);
            // polar pattern ghost: |alpha + (1 - alpha) cos theta|, 5 cm across
            const double alpha = p[kMicPattern];
            juce::Path pol;
            for (int i = 0; i <= 72; ++i) {
                const double th = 2 * M_PI * i / 72.0;
                const double rho = 0.03 * std::abs(alpha + (1 - alpha) * std::cos(th));
                const double dz = ax * std::cos(th) - ar * std::sin(th), dr = ar * std::cos(th) + ax * std::sin(th);
                const auto q = P(zm + rho * dz, off + rho * dr);
                if (i == 0) pol.startNewSubPath(q); else pol.lineTo(q);
            }
            g.setColour(accent.withAlpha(0.45f));
            float dash[] = { 3.0f, 3.0f };
            juce::PathStrokeType(1.0f).createDashedStroke(pol, pol, dash, 2);
            g.fillPath(pol);
            juce::String ml = "mic " + juce::String(dist * 100, 1) + " cm out, " + juce::String(off * 100, 1) + " cm across";
            if (std::abs(ang) > 0.01) ml << ", " << juce::String(juce::roundToInt(ang * 180 / M_PI)) << " deg";
            ml << ", " << juce::String(juce::roundToInt(cap * 1000)) << " mm capsule";
            if (beyond) ml << " (further than shown)";
            label(g, ml, c + juce::Point<float>(-40, (float) (bw * sc) + 10));
        }

        // ---- legend: the cone's material
        juce::String mat;
        mat << "paper " << juce::String(thick * 1000, 2) << " mm";
        if (taper > 1.05) mat << " (x" << juce::String(taper, 2) << " at the neck)";
        mat << ", " << juce::String(p[kYoungs] / 1e9, 1) << " GPa, " << juce::String(juce::roundToInt(p[kDensity])) << " kg/m3, loss "
            << juce::String(p[kLoss], 3);
        if (p[kRibs] > 1.2) mat << ", ribs x" << juce::String(p[kRibs], 1);
        label(g, mat, { area.getX() + 4, area.getY() + 2 });
        label(g, "dust cap " + juce::String(p[kDustCap] * 200, 1) + " cm across, " + juce::String(p[kCapMass] * 1e3, 2) + " g;  Bl "
                     + juce::String(p[kBl], 1) + " T m;  surround damping " + juce::String(p[kSurroundR], 2),
              { area.getX() + 4, area.getY() + 18 });
    }

    // ------------------------------------------------------------------ the response
    void drawResponse(juce::Graphics& g, juce::Rectangle<float> r)
    {
        plotArea = r;
        g.setColour(juce::Colour(0xff232529));
        g.fillRect(r);
        if (disp.freq.empty()) return;
        const double f0 = disp.freq.front(), f1 = disp.freq.back();
        double top = -1e9;
        for (double v : disp.spl) top = std::max(top, v);
        top = std::ceil(top / 5) * 5 + 2;
        const double span = 40;
        auto X = [&](double f) { return r.getX() + r.getWidth() * (float) (std::log(f / f0) / std::log(f1 / f0)); };
        auto Y = [&](double db) { return r.getY() + r.getHeight() * (float) ((top - db) / span); };
        g.setFont(juce::FontOptions(11.0f));
        for (double f : { 100.0, 1000.0, 10000.0 }) {
            g.setColour(dim.withAlpha(0.25f));
            g.drawVerticalLine((int) X(f), r.getY(), r.getBottom());
            g.setColour(dim);
            g.drawText(hz(f), juce::Rectangle<float>(X(f) + 2, r.getBottom() - 14, 60, 12), juce::Justification::left);
        }
        for (double db = top - span; db <= top; db += 10) {
            g.setColour(dim.withAlpha(0.15f));
            g.drawHorizontalLine((int) Y(db), r.getX(), r.getRight());
        }
        juce::Path c;
        for (size_t i = 0; i < disp.freq.size(); ++i) {
            const auto q = juce::Point<float>(X(disp.freq[i]), juce::jlimit(r.getY(), r.getBottom(), Y(disp.spl[i])));
            if (i == 0) c.startNewSubPath(q); else c.lineTo(q);
        }
        g.setColour(accent);
        g.strokePath(c, juce::PathStrokeType(1.6f));
        // cursor
        const float xc = X(juce::jlimit(f0, f1, cursorHz));
        g.setColour(juce::Colour(0xff5fb3ff));
        g.drawVerticalLine((int) xc, r.getY(), r.getBottom());
        double lv = disp.spl.front();
        for (size_t i = 0; i + 1 < disp.freq.size(); ++i)
            if (disp.freq[i] <= cursorHz && disp.freq[i + 1] > cursorHz) lv = disp.spl[i];
        g.setColour(fg);
        g.drawText("at the mic, 2.83 V: " + juce::String(lv, 1) + " dB SPL @ " + hz(cursorHz) + "   (drag to move)",
                   r.reduced(6, 4), juce::Justification::topLeft);
    }

    void label(juce::Graphics& g, const juce::String& t, juce::Point<float> at)
    {
        g.setFont(juce::FontOptions(12.0f));
        g.setColour(fg.withAlpha(0.85f));
        g.drawText(t, juce::Rectangle<float>(at.x, at.y, 900, 14), juce::Justification::left);
    }
    static juce::String hz(double f) { return f >= 1000 ? juce::String(f / 1000, f < 10000 ? 2 : 1) + " kHz" : juce::String(juce::roundToInt(f)) + " Hz"; }
};

} // namespace cd::ui
