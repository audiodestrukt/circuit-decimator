#include "CircuitView.h"

namespace {

constexpr float kW = 1000, kH = 620;   // virtual canvas; scaled to fit

const auto kBg = juce::Colour(0xff1b1c1e);
const auto kPart = juce::Colour(0xffc9ccd1);
const auto kHurt = juce::Colour(0xffff4d3d);
const auto kGlow = juce::Colour(0xff7fd18b);
const auto kDim = juce::Colour(0xff8a8f98);
const auto kGround = juce::Colour(0xff5a5e66);

// Board style: the materials of an old pedal's guts
const auto kPhenolic = juce::Colour(0xff3a2618);
const auto kCopper = juce::Colour(0xffe0915a);
const auto kCopperHot = juce::Colour(0xfff6c79a);
const auto kVerdigris = juce::Colour(0xff6fb8a0);
const auto kSilk = juce::Colour(0xffe9e4d8);
const auto kSolder = juce::Colour(0xffb9bdc2);
const auto kScorch = juce::Colour(0xffff5a36);

float clamp01(double v) { return (float) juce::jlimit(0.0, 1.0, v); }

// how far a log-scaled knob sits from its healthy value, 0..1
float logDeviation(double v, double healthy, double worstRatio)
{
    return clamp01(std::abs(std::log(v / healthy)) / std::log(worstRatio));
}

} // namespace

CircuitView::CircuitView(cd::FuzzEngine& e) : engine(e)
{
    setOpaque(true);
    startTimerHz(30);
}

void CircuitView::setStyle(Style s)
{
    style = s;
    resized();
    repaint();
}

void CircuitView::setPatina(float amount)
{
    patina = juce::jlimit(0.0f, 1.0f, amount);
}

void CircuitView::setTypeface(juce::Typeface::Ptr t)
{
    typeface = std::move(t);
}

// Phenolic board: warm brown with fibre speckle and a soft vignette, rendered
// once per size so painting stays cheap.
void CircuitView::resized()
{
    if (!board() || getWidth() <= 0 || getHeight() <= 0) { boardTexture = {}; return; }
    boardTexture = juce::Image(juce::Image::RGB, getWidth(), getHeight(), true);
    juce::Graphics g(boardTexture);
    g.fillAll(kPhenolic);
    juce::Random rng(1966);
    const int specks = getWidth() * getHeight() / 30;
    for (int i = 0; i < specks; ++i) {
        const float x = rng.nextFloat() * (float) getWidth(), y = rng.nextFloat() * (float) getHeight();
        g.setColour((rng.nextBool() ? juce::Colours::black : juce::Colour(0xffc08050)).withAlpha(0.05f + 0.07f * rng.nextFloat()));
        g.fillRect(x, y, 1.0f + rng.nextFloat() * 2.0f, 1.0f);
    }
    const auto r = getLocalBounds().toFloat();
    juce::ColourGradient vignette(juce::Colours::transparentBlack, r.getCentreX(), r.getCentreY(),
                                  juce::Colours::black.withAlpha(0.45f), r.getX(), r.getY(), true);
    g.setGradientFill(vignette);
    g.fillRect(r);
}

void CircuitView::timerCallback()
{
    for (int i = 0; i < cd::kNumKnobs; ++i)
        knobs.v[i] = engine.knobsInUse[(size_t) i].load();
    // smooth the telemetry so the colours breathe rather than flicker
    for (size_t k = 0; k < vMean.size(); ++k) {
        const float m = engine.nodeMean[k].load();
        const float s = engine.nodeMax[k].load() - engine.nodeMin[k].load();
        vMean[k] += 0.3f * (m - vMean[k]);
        vSwing[k] += (s > vSwing[k] ? 0.5f : 0.1f) * (s - vSwing[k]);
    }
    repaint();
}

float CircuitView::mean(int node) const { return node == GND ? 0.0f : vMean[(size_t) node]; }
float CircuitView::swing(int node) const { return node == GND ? 0.0f : vSwing[(size_t) node]; }

juce::Colour CircuitView::voltageColour(float volts) const
{
    const float t = clamp01(volts / 9.0);
    const auto blue = juce::Colour(0xff3a6fd8), teal = juce::Colour(0xff3fbf8f), amber = juce::Colour(0xffffb347);
    return t < 0.5f ? blue.interpolatedWith(teal, t * 2) : teal.interpolatedWith(amber, (t - 0.5f) * 2);
}

juce::Colour CircuitView::partColour(float damage) const
{
    return board() ? kSilk.withAlpha(0.88f).interpolatedWith(kScorch, damage) : kPart.interpolatedWith(kHurt, damage);
}

juce::Colour CircuitView::traceColour(int node) const
{
    const auto base = kCopper.interpolatedWith(kVerdigris, patina);
    if (node == GND) return base.darker(0.35f);
    return base.interpolatedWith(kCopperHot, clamp01(swing(node) / 1.5));
}

juce::Colour CircuitView::groundColour() const
{
    return board() ? kCopper.interpolatedWith(kVerdigris, patina).darker(0.35f) : kGround;
}

juce::Colour CircuitView::textColour() const
{
    return board() ? kSilk.withAlpha(0.7f) : kDim;
}

// ---------------------------------------------------------------- primitives
void CircuitView::wire(juce::Graphics& g, std::initializer_list<Pt> pts, int node) const
{
    juce::Path p;
    bool first = true;
    for (auto pt : pts) {
        if (first) p.startNewSubPath(pt); else p.lineTo(pt);
        first = false;
    }
    if (board()) {
        // copper trace: warm bloom where the signal is, then the trace itself
        const float s = node == GND ? 0.0f : clamp01(swing(node) / 1.5);
        if (s > 0.02f) {
            g.setColour(kCopperHot.withAlpha(0.10f + 0.30f * s));
            g.strokePath(p, juce::PathStrokeType(6 + 14 * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour(traceColour(node));
        g.strokePath(p, juce::PathStrokeType(5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return;
    }
    if (node != GND) {
        const float s = clamp01(swing(node) / 2.0);
        if (s > 0.01f) {
            g.setColour(kGlow.withAlpha(0.15f + 0.35f * s));
            g.strokePath(p, juce::PathStrokeType(3 + 12 * s, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour(voltageColour(mean(node)));
    } else {
        g.setColour(kGround);
    }
    g.strokePath(p, juce::PathStrokeType(2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void CircuitView::dot(juce::Graphics& g, Pt p, int node) const
{
    if (board()) {   // solder joint on a pad
        g.setColour(traceColour(node));
        g.fillEllipse(p.x - 8, p.y - 8, 16, 16);
        g.setColour(kSolder);
        g.fillEllipse(p.x - 5.5f, p.y - 5.5f, 11, 11);
        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.fillEllipse(p.x - 3.5f, p.y - 3.5f, 3.5f, 3.5f);
        return;
    }
    g.setColour(node == GND ? kGround : voltageColour(mean(node)));
    g.fillEllipse(p.x - 4, p.y - 4, 8, 8);
}

void CircuitView::ground(juce::Graphics& g, Pt p) const
{
    g.setColour(board() ? kSilk.withAlpha(0.6f) : kGround);
    g.drawLine(p.x, p.y, p.x, p.y + 10, 2);
    for (int i = 0; i < 3; ++i) {
        const float w = 14.0f - 5.0f * (float) i, y = p.y + 10 + 5.0f * (float) i;
        g.drawLine(p.x - w, y, p.x + w, y, 2);
    }
}

void CircuitView::resistor(juce::Graphics& g, Pt a, Pt b, float damage, float alpha, bool dashed) const
{
    const auto d = b - a;
    const float len = d.getDistanceFromOrigin();
    const auto u = d / len, n = Pt(-u.y, u.x);
    juce::Path p;
    p.startNewSubPath(a);
    const float body = juce::jmin(60.0f, len * 0.7f), start = (len - body) / 2;
    p.lineTo(a + u * start);
    for (int i = 0; i < 6; ++i)
        p.lineTo(a + u * (start + body * ((float) i + 0.5f) / 6) + n * (i % 2 ? -8.0f : 8.0f));
    p.lineTo(a + u * (start + body));
    p.lineTo(b);
    const auto c = partColour(damage).withMultipliedAlpha(alpha);
    if (damage > 0.3f) {   // scorch halo
        g.setColour((board() ? kScorch : kHurt).withAlpha((board() ? 0.35f : 0.25f) * damage * alpha));
        g.strokePath(p, juce::PathStrokeType(board() ? 12.0f : 8.0f));
    }
    g.setColour(c);
    juce::PathStrokeType st(2.2f);
    if (dashed) {
        juce::Path dp;
        const float dashes[] = { 5, 4 };
        st.createDashedStroke(dp, p, dashes, 2);
        g.fillPath(dp);
    } else {
        g.strokePath(p, st);
    }
}

void CircuitView::capacitor(juce::Graphics& g, Pt a, Pt b, float damage) const
{
    const auto d = b - a;
    const float len = d.getDistanceFromOrigin();
    const auto u = d / len, n = Pt(-u.y, u.x);
    const auto m = a + d * 0.5f, p1 = m - u * 5, p2 = m + u * 5;
    g.setColour(partColour(damage));
    g.drawLine({ a, p1 }, 2.2f);
    g.drawLine({ p2, b }, 2.2f);
    g.drawLine({ p1 - n * 14, p1 + n * 14 }, 3);
    g.drawLine({ p2 - n * 14, p2 + n * 14 }, 3);
}

void CircuitView::inductor(juce::Graphics& g, Pt a, Pt b) const
{
    juce::Path p;
    p.startNewSubPath(a);
    const float dx = (b.x - a.x) / 4;
    for (int i = 0; i < 4; ++i) {
        const float x0 = a.x + dx * (float) i;
        p.cubicTo(x0, a.y - 14, x0 + dx, a.y - 14, x0 + dx, a.y);
    }
    g.setColour(partColour(0));
    g.strokePath(p, juce::PathStrokeType(2.2f));
}

// NPN, base on the left at `base`; collector leaves up from (base.x+40, base.y-30),
// emitter down from (base.x+40, base.y+30)
void CircuitView::transistor(juce::Graphics& g, Pt base, float damage, float tempC) const
{
    const auto centre = base + Pt(24, 0);
    // temperature tint: icy below 27 C, warm above
    const float t = (float) juce::jlimit(-1.0, 1.0, (tempC - 27.0) / 70.0);
    const auto tint = t >= 0 ? juce::Colour(0xffff8a3d).withAlpha(0.35f * t) : juce::Colour(0xff7fc8ff).withAlpha(-0.35f * t);
    g.setColour(tint);
    g.fillEllipse(centre.x - 30, centre.y - 34, 60, 68);
    g.setColour(partColour(damage));
    g.drawEllipse(centre.x - 30, centre.y - 34, 60, 68, 1.6f);
    g.drawLine(base.x, base.y, base.x + 18, base.y, 2.2f);
    g.drawLine(base.x + 18, base.y - 18, base.x + 18, base.y + 18, 3.5f);
    g.drawLine(base.x + 18, base.y - 8, base.x + 40, base.y - 30, 2.2f);
    g.drawLine(base.x + 18, base.y + 8, base.x + 40, base.y + 30, 2.2f);
    juce::Path arrow;
    arrow.addArrow({ base.x + 26, base.y + 15.3f, base.x + 40, base.y + 30 }, 0, 10, 9);
    g.fillPath(arrow);
}

void CircuitView::battery(juce::Graphics& g, Pt top, Pt bottom, float damage) const
{
    const float mid = (top.y + bottom.y) / 2;
    g.setColour(partColour(damage));
    g.drawLine(top.x, top.y, top.x, mid - 8, 2.2f);
    g.drawLine(top.x, mid + 8, top.x, bottom.y, 2.2f);
    g.drawLine(top.x - 20, mid - 8, top.x + 20, mid - 8, 3);
    g.drawLine(top.x - 10, mid, top.x + 10, mid, 3);
    g.drawLine(top.x - 20, mid + 8, top.x + 20, mid + 8, 3);
    // charge gauge
    const float charge = clamp01(knobs[cd::kBattery] / 12.0);
    g.setColour(textColour());
    g.drawRect(juce::Rectangle<float>(top.x + 30, mid - 24, 10, 48), 1.0f);
    g.setColour(board() ? kCopper.interpolatedWith(kScorch, damage) : voltageColour((float) knobs[cd::kBattery]));
    g.fillRect(top.x + 31, mid + 23 - 46 * charge, 8.0f, 46 * charge);
}

void CircuitView::wiper(juce::Graphics& g, Pt tip, Pt from, float damage) const
{
    juce::Path a;
    a.addArrow({ from, tip }, 2, 11, 10);
    g.setColour(partColour(damage));
    g.fillPath(a);
}

void CircuitView::label(juce::Graphics& g, const juce::String& text, Pt p, float size, juce::Colour c,
                        juce::Justification just) const
{
    g.setColour(c.isTransparent() ? textColour() : c);
    g.setFont(typeface != nullptr ? juce::Font(juce::FontOptions(typeface).withHeight(size)) : juce::Font(juce::FontOptions(size)));
    const float w = 170;
    const float x = just.testFlags(juce::Justification::right) ? p.x - w
                    : just.testFlags(juce::Justification::horizontallyCentred) ? p.x - w / 2 : p.x;
    g.drawText(text, juce::Rectangle<float>(x, p.y - size, w, size * 2), just, false);
}

// ---------------------------------------------------------------- schematic
void CircuitView::paint(juce::Graphics& g)
{
    if (board() && boardTexture.isValid()) g.drawImageAt(boardTexture, 0, 0);
    else g.fillAll(kBg);
    scale = juce::jmin((float) getWidth() / kW, (float) getHeight() / kH);
    const float ox = ((float) getWidth() - kW * scale) / 2, oy = ((float) getHeight() - kH * scale) / 2;
    g.addTransform(juce::AffineTransform::scale(scale).translated(ox, oy));

    using N = cd::FuzzFace;
    const auto& k = knobs;
    const float vcc = (float) k[cd::kBattery];

    // damage per part, 0 = healthy
    const float dBattery = k[cd::kBattery] < 9 ? clamp01((9 - k[cd::kBattery]) / 5.5) : clamp01((k[cd::kBattery] - 9) / 6);
    const float dSag = clamp01(std::log10(k[cd::kBatteryRes]) / std::log10(5000.0));
    const float dBias = logDeviation(k[cd::kBias], 5.6, 15 / 5.6);
    const float dQ1 = juce::jmax(logDeviation(k[cd::kQ1Gain], 250, 50), (float) k[cd::kJunctionLeak]);
    const float dQ2 = juce::jmax(logDeviation(k[cd::kQ2Gain], 250, 50), (float) k[cd::kJunctionLeak]);
    const float dCin = (float) k[cd::kCapLeak];
    const float dBypass = clamp01(std::log(20 / k[cd::kBypassCap]) / std::log(200.0));
    const float tempC = (float) k[cd::kTemperature];

    // ---- supply rail + battery
    wire(g, { { 330, 40 }, { 830, 40 } }, N::VP);
    resistor(g, { 830, 40 }, { 920, 40 }, dSag);
    // battery + terminal: the EMF, before the sag resistor
    if (board()) wire(g, { { 920, 40 }, { 920, 170 } }, N::VP);
    else { g.setColour(voltageColour(vcc)); g.drawLine(920, 40, 920, 170, 2.2f); }
    battery(g, { 920, 170 }, { 920, 240 }, dBattery);
    wire(g, { { 920, 240 }, { 920, 280 } }, GND);
    ground(g, { 920, 280 });
    capacitor(g, { 780, 40 }, { 780, 130 }, 0);
    ground(g, { 780, 130 });
    dot(g, { 780, 40 }, N::VP);

    // ---- pickup + input
    g.setColour(partColour(0));
    g.drawEllipse(54, 424, 32, 32, 2.2f);
    label(g, "~", { 70, 440 }, 20, partColour(0), juce::Justification::centred);
    wire(g, { { 70, 456 }, { 70, 490 } }, GND);
    ground(g, { 70, 490 });
    wire(g, { { 70, 424 }, { 70, 380 }, { 90, 380 } }, N::P1);
    inductor(g, { 90, 380 }, { 150, 380 });
    wire(g, { { 150, 380 }, { 180, 380 } }, N::GIN);
    capacitor(g, { 165, 380 }, { 165, 440 }, 0);
    ground(g, { 165, 440 });
    dot(g, { 165, 380 }, N::GIN);
    capacitor(g, { 180, 380 }, { 240, 380 }, dCin);
    if (k[cd::kCapLeak] > 0.001) {  // leak path around the cap, fading in with the knob
        const float a = 0.35f + 0.65f * dCin;
        g.setColour(partColour(dCin).withAlpha(a));
        g.drawLine(185, 380, 185, 350, 1.5f);
        g.drawLine(235, 380, 235, 350, 1.5f);
        resistor(g, { 185, 350 }, { 235, 350 }, dCin, a, true);
    }

    // ---- Q1
    wire(g, { { 240, 380 }, { 290, 380 } }, N::B1);
    dot(g, { 240, 380 }, N::B1);
    transistor(g, { 290, 380 }, dQ1, tempC);
    wire(g, { { 330, 350 }, { 330, 260 } }, N::C1);
    wire(g, { { 330, 410 }, { 330, 450 } }, GND);
    ground(g, { 330, 450 });
    resistor(g, { 330, 40 }, { 330, 260 }, 0);
    dot(g, { 330, 40 }, N::VP);
    if (k[cd::kJunctionLeak] > 0.001)
        resistor(g, { 318, 270 }, { 266, 372 }, (float) k[cd::kJunctionLeak],
                 0.35f + 0.65f * (float) k[cd::kJunctionLeak], true);

    // ---- Q2
    wire(g, { { 330, 260 }, { 450, 260 } }, N::C1);
    dot(g, { 330, 260 }, N::C1);
    transistor(g, { 450, 260 }, dQ2, tempC);
    wire(g, { { 490, 230 }, { 490, 210 } }, N::C2);
    resistor(g, { 490, 210 }, { 490, 140 }, 0);
    resistor(g, { 490, 140 }, { 490, 40 }, dBias);
    wiper(g, { 480, 90 }, { 440, 110 }, dBias);   // trimmer arrow
    dot(g, { 490, 40 }, N::VP);
    dot(g, { 490, 140 }, N::TAP);
    if (k[cd::kJunctionLeak] > 0.001)
        resistor(g, { 505, 212 }, { 420, 262 }, (float) k[cd::kJunctionLeak],
                 0.35f + 0.65f * (float) k[cd::kJunctionLeak], true);

    // ---- emitter: fuzz pot + bypass cap, feedback to Q1 base
    wire(g, { { 490, 290 }, { 490, 340 }, { 580, 340 }, { 580, 350 } }, N::E2);
    dot(g, { 490, 340 }, N::E2);
    resistor(g, { 580, 350 }, { 580, 470 }, 0);
    ground(g, { 580, 470 });
    const float wy = 380 + (1 - (float) k[cd::kFuzz]) * 60;
    wiper(g, { 590, wy }, { 630, wy }, 0);
    wire(g, { { 630, wy }, { 660, wy }, { 660, 400 } }, N::FZW);
    capacitor(g, { 660, 400 }, { 660, 460 }, dBypass);
    ground(g, { 660, 460 });
    wire(g, { { 490, 340 }, { 490, 530 }, { 420, 530 } }, N::E2);
    resistor(g, { 420, 530 }, { 290, 530 }, 0);
    wire(g, { { 290, 530 }, { 240, 530 }, { 240, 380 } }, N::B1);

    // ---- output: Cout -> volume pot -> OUT
    wire(g, { { 490, 140 }, { 560, 140 } }, N::TAP);
    capacitor(g, { 560, 140 }, { 620, 140 }, 0);
    wire(g, { { 620, 140 }, { 700, 140 }, { 700, 160 } }, N::O1);
    resistor(g, { 700, 160 }, { 700, 290 }, 0);
    ground(g, { 700, 290 });
    const float vy = 190 + (1 - (float) k[cd::kVolume]) * 70;
    wiper(g, { 710, vy }, { 750, vy }, 0);
    wire(g, { { 750, vy }, { 820, vy } }, N::OUT);
    if (board()) dot(g, { 826, vy }, N::OUT);
    else { g.setColour(voltageColour(mean(N::OUT))); g.drawEllipse(820, vy - 6, 12, 12, 2.2f); }

    if (board()) {
        // silkscreen values, plus the two readouts a player cares about
        const auto silk = kSilk.withAlpha(0.72f);
        const auto hot = kScorch;
        label(g, "33k", { 350, 150 }, 20, silk);
        label(g, "470", { 508, 175 }, 20, silk);
        label(g, "100k", { 355, 562 }, 20, silk, juce::Justification::centred);
        label(g, juce::String::fromUTF8("2.2\xc2\xb5"), { 210, 418 }, 20, dCin > 0.3f ? hot : silk, juce::Justification::centred);
        label(g, "10n", { 590, 110 }, 20, silk, juce::Justification::centred);
        label(g, "Q1", { 354, 394 }, 24, dQ1 > 0.3f ? hot : silk);
        label(g, "Q2", { 524, 258 }, 24, dQ2 > 0.3f ? hot : silk);
        label(g, "out", { 846, vy }, 22, silk);
        label(g, "in", { 38, 440 }, 22, silk, juce::Justification::right);
        label(g, juce::String(mean(N::VP), 1) + " V", { 900, 135 }, 26, dBattery > 0.3f ? hot : silk.withAlpha(0.95f),
              juce::Justification::right);
        const float vce = mean(N::C2) - mean(N::E2), headroom = mean(N::VP) - mean(N::C2);
        const bool starved = vce < 0.3f, cut = headroom < 0.15f * juce::jmax(0.5f, mean(N::VP));
        if (starved || cut)
            label(g, starved ? "Q2 starved" : "Q2 cut off", { 524, 286 }, 20, hot);
        return;
    }

    // ---- values
    const auto val = juce::Colour(0xffb8bcc4);
    label(g, "Battery Sag " + juce::String(juce::roundToInt(k[cd::kBatteryRes])) + juce::String::fromUTF8(" \xce\xa9"), { 875, 18 }, 12, dSag > 0.3f ? kHurt : val, juce::Justification::centred);
    label(g, juce::String(vcc, 1) + " V battery", { 895, 205 }, 13, dBattery > 0.3f ? kHurt : val, juce::Justification::right);
    label(g, "supply " + juce::String(k[cd::kSupplyCap], 1) + juce::String::fromUTF8(" \xc2\xb5" "F"), { 795, 90 }, 12, val);
    label(g, "pickup", { 120, 355 }, 12, val, juce::Justification::centred);
    label(g, juce::String::fromUTF8("Cin 2.2 \xc2\xb5" "F"), { 210, 325 }, 12, dCin > 0.3f ? kHurt : val, juce::Justification::centred);
    label(g, "33k", { 345, 150 }, 12, val);
    label(g, "470", { 505, 175 }, 12, val);
    label(g, "Bias Trim " + juce::String(k[cd::kBias], 1) + "k", { 435, 60 }, 12, dBias > 0.3f ? kHurt : val, juce::Justification::right);
    label(g, "100k", { 355, 552 }, 12, val, juce::Justification::centred);
    label(g, "Fuzz " + juce::String(k[cd::kFuzz], 2), { 595, 490 }, 12, val);
    label(g, "bypass " + juce::String(k[cd::kBypassCap], 1) + juce::String::fromUTF8(" \xc2\xb5" "F"), { 675, 430 }, 12, dBypass > 0.3f ? kHurt : val);
    label(g, "10n", { 590, 115 }, 12, val, juce::Justification::centred);
    label(g, "Volume " + juce::String(k[cd::kVolume], 2), { 715, 305 }, 12, val);
    label(g, "OUT", { 840, vy }, 14, kPart);
    const auto hfe = [&](int i) { return "hFE " + juce::String(juce::roundToInt(k[i])); };
    const auto temp = juce::String(juce::roundToInt(tempC)) + juce::String::fromUTF8(" \xc2\xb0" "C");
    label(g, "Q1  " + hfe(cd::kQ1Gain), { 352, 392 }, 12, dQ1 > 0.3f ? kHurt : val);
    label(g, temp, { 352, 410 }, 12, val);
    label(g, "Q2  " + hfe(cd::kQ2Gain), { 522, 258 }, 12, dQ2 > 0.3f ? kHurt : val);
    label(g, temp, { 522, 276 }, 12, val);

    // ---- live readouts
    const auto volts = [&](int n) { return juce::String(mean(n), 2) + " V"; };
    label(g, "rail " + volts(N::VP), { 600, 22 }, 13, voltageColour(mean(N::VP)), juce::Justification::centred);
    label(g, "b " + volts(N::B1), { 248, 470 }, 12, voltageColour(mean(N::B1)));
    label(g, "c " + volts(N::C1), { 340, 285 }, 12, voltageColour(mean(N::C1)));
    label(g, "e " + volts(N::E2), { 500, 322 }, 12, voltageColour(mean(N::E2)));
    // Q2's operating point decides whether the fuzz sings, gates or dies
    const float vce = mean(N::C2) - mean(N::E2), headroom = mean(N::VP) - mean(N::C2);
    const bool saturated = vce < 0.3f, cutoff = headroom < 0.15f * juce::jmax(0.5f, mean(N::VP));
    const auto state = saturated ? juce::String("saturated") : cutoff ? juce::String("cut off") : juce::String("biased");
    label(g, "c " + volts(N::C2) + "  " + state, { 505, 222 }, 13,
          saturated || cutoff ? kHurt : voltageColour(mean(N::C2)));

    // ---- legend
    const float lx = 760, ly = 585;
    for (int i = 0; i < 100; ++i) {
        g.setColour(voltageColour(9.0f * (float) i / 99));
        g.fillRect(lx + 1.8f * (float) i, ly, 1.9f, 8.0f);
    }
    label(g, "0 V", { lx, ly + 20 }, 11);
    label(g, "9 V", { lx + 180, ly + 20 }, 11, {}, juce::Justification::right);
    g.setColour(kGlow.withAlpha(0.5f));
    g.fillRoundedRectangle(lx - 200, ly - 1, 30, 10, 5);
    label(g, "signal", { lx - 164, ly + 4 }, 11);
    g.setColour(kHurt);
    g.fillRoundedRectangle(lx - 110, ly - 1, 30, 10, 5);
    label(g, "damage", { lx - 74, ly + 4 }, 11);
}
