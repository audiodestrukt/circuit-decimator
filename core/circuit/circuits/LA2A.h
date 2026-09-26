// LA2A.h -- LA-2A-style optical leveling amplifier as a netlist for the
// general engine. Mirrors sim/la2a/la2a.cir (the spec). Topology from the
// Universal Audio LA-2A manual, figs. 5-7:
//
//   600 ohm line -> input transformer (1:4, JA core) -> R6 68k -> j -> R7 2.7k
//   -> att, where the T4 photocell shunts to ground -> Gain pot (100k) ->
//   V1A / V1B 12AX7 voltage amplifier -> V2A 12BH7 cathode follower over a
//   V2B current sink -> 10 uF -> output transformer (3:1 down, JA core) -> 600 ohm.
//   Negative feedback from the cathode follower into V1A's cathode.
//
//   Sidechain (fed from j, so it sees the gain-reduced signal: feedback
//   leveling): Peak Reduction pot (100k) -> V3 12AX7 cathode-coupled pair ->
//   V4 6AQ5 driver -> 0.1 uF -> EL panel of the T4 cell.
//
// Assumptions (marked where the manual's figures are ambiguous or silent):
//   - transformer ratios, inductances and cores (HA-100X, A-24 not specified)
//   - one ideal B+ rail for everything; screen of V4 straight to B+
//   - V3 tail resistor, V3B grid returned to ground (1M || C8)
//   - the sidechain emphasis (R37/C12) at its flat setting, so left out
//   - C3 (V2 bootstrap) and the metering cell left out
//   - the feedback resistor rfb trims the overall gain to UA's 40 dB
//   - EL panel electrical load: 10 nF || 470k
//   - V4's screen fed through R35 220k, bypassed by C6 .01
//
// Two circuits, solved in turn each sample: the audio path (c) and the
// sidechain (sc). They meet only at j (the sidechain's input is the Peak
// Reduction pot, loading j with its 100k, driven by j's voltage -- exact up to
// V3A's grid current) and at the T4 (the cell in c, its EL panel in sc; light
// moves in commit(), so the cell sees the panel one sample late: 5 us against
// the panel's 1 ms). The sidechain can run at a submultiple of the audio rate
// (scEvery): what it carries to the panel is band-limited by the oversampler.
#pragma once

#include "../Devices.h"

#include <map>
#include <string>

namespace cd {

struct LA2AXfmr {
    double n, rp, llp, rs, lls, cs;
    double np, ac, le, lg;
    double ms, a, alpha, k, c;
    bool linearCore;
    double lm;
};

struct LA2AParams {
    double rsrc = 600;
    // input transformer (1:4 step-up, nickel-ish core; ~20 H primary)
    LA2AXfmr in { 4, 30, 1e-3, 600, 16e-3, 200e-12, 1500, 1.2e-4, 0.08, 0, 6.4e5, 8.5, 1e-5, 4, 0.2, false, 21 };
    // output transformer (3:1 step-down, silicon steel, cap-coupled so no DC)
    LA2AXfmr out { 1.0 / 3, 250, 5e-3, 25, 0.5e-3, 100e-12, 2600, 2e-4, 0.12, 0, 1.3e6, 100, 1e-4, 20, 0.3, false, 18 };
    double rload = 600;
    // attenuator (fig. 5)
    double r5 = 68e3, r6 = 68e3, r7 = 2.7e3;
    double gainPot = 100e3, gain = 0.75;         // wiper position 0..1
    // voltage amplifier (fig. 7)
    double r9 = 220e3, r10 = 1.5e3, c1 = 0.02e-6, r12 = 470e3, r13 = 220e3, r14 = 2.7e3, c2 = 0.1e-6;
    double rfb = 470e3, cfb = 1e-6;              // feedback, V2A cathode -> V1A cathode (assumed, R11 + block)
    // cathode follower + sink
    double r15 = 470e3, r17 = 10e3, r18 = 1e3, r19 = 470e3, r20 = 1e3, c5 = 10e-6;
    // sidechain (fig. 6)
    double peakPot = 100e3, peak = 0.5;          // Peak Reduction wiper 0..1
    double r31 = 1e3, r32 = 1e3, rtail = 2.2e3, r33 = 220e3, c8 = 0.03e-6, rg3b = 1e6;
    double c9 = 0.02e-6, r3 = 1e6, r34 = 22e3, r36 = 1e3, c10 = 50e-6, c11 = 0.1e-6;
    double cel = 10e-9, rel = 470e3, r35 = 220e3, c6 = 0.01e-6;
    double c3 = 0.1e-6;                          // V2A plate -> V2B grid (White cathode follower)
    double bplus = 250;
    double cmiller = 1.7e-12;                    // 12AX7 grid-plate
};

struct LA2A {
    net::Circuit c;    // audio path
    net::Circuit sc;   // sidechain
    int input = -1, out = -1, supply = -1, scInput = -1, scSupply = -1, jNode = -1, elNode = -1;
    int scEvery = 1, scPhase = 0;
    net::JACore* coreIn = nullptr;
    net::JACore* coreOut = nullptr;
    net::T4Cell* t4 = nullptr;
    net::Triode* v1a = nullptr;
    std::map<std::string, int> el;     // audio path: component name -> element index (for setValueDeferred)
    std::map<std::string, int> scel;   // sidechain
    LA2AParams prm;                    // as built

    void prepare(double sampleRate, int sidechainEvery = 1)
    {
        scEvery = std::max(1, sidechainEvery);
        scPhase = 0;
        c.prepare(sampleRate);
        sc.prepare(sampleRate / scEvery);
        t4->setPanelVoltage(sc.x(elNode));
    }
    void setSupply(double v)
    {
        c.setInput(supply, v);
        sc.setInput(scSupply, v);
    }
    // one sample of the audio path; the sidechain every scEvery samples
    void process(double in)
    {
        c.setInput(input, in);
        c.process();
        if (++scPhase >= scEvery) {
            scPhase = 0;
            sc.setInput(scInput, c.x(jNode));
            sc.process();
            t4->setPanelVoltage(sc.x(elNode));
        }
    }
    double output() const { return c.out(out); }

    static net::Triode ax7() { return {}; }
    static net::Triode bh7()
    {
        net::Triode t;   // 12AU7 curve shape, mu 16.5, scaled to 250 V / -10.5 V -> 11.5 mA
        t.mu = 16.5; t.ex = 1.3; t.kg1 = 1497; t.kp = 84; t.kvb = 300;
        return t;
    }

    static std::unique_ptr<net::JACore> core(const LA2AXfmr& x)
    {
        auto j = std::make_unique<net::JACore>();
        j->np = x.np; j->ac = x.ac; j->le = x.le; j->lg = x.lg;
        j->ms = x.ms; j->a = x.a; j->alpha = x.alpha; j->k = x.k; j->c = x.c;
        j->linear = x.linearCore; j->lm = x.lm;
        return j;
    }

    void setGain(double g, const LA2AParams& p)
    {
        g = std::clamp(g, 1e-4, 1 - 1e-4);
        c.setValueDeferred(el["r1a"], p.gainPot * (1 - g));
        c.setValueDeferred(el["r1b"], p.gainPot * g);
    }
    void setPeak(double g, const LA2AParams& p)
    {
        g = std::clamp(g, 1e-4, 1 - 1e-4);
        sc.setValueDeferred(scel["r2a"], p.peakPot * (1 - g));
        sc.setValueDeferred(scel["r2b"], p.peakPot * g);
    }

    // Attenuator transfer s7 -> att for a cell resistance: the input
    // transformer's Thevenin source, R6, the Peak Reduction pot's load at j,
    // R7, the cell shunted by the Gain pot. Its ratio to the dark value is the
    // gain reduction (the real unit meters it the same way, from a second,
    // matched cell).
    double attenuation(double rcell) const
    {
        auto par = [](double a, double b) { return a * b / (a + b); };
        const double r5 = c.value(el.at("r5")), r6 = c.value(el.at("r6")), r7 = c.value(el.at("r7"));
        const double rth = par(prm.rsrc * prm.in.n * prm.in.n + prm.in.rs, r5);
        const double za = par(rcell, prm.gainPot), zb = r7 + za, zj = par(prm.peakPot, zb);
        return zj / (rth + r6 + zj) * za / zb;
    }
    double gainReductionDb() const
    {
        return 20 * std::log10(attenuation(t4->rDark) / attenuation(t4->resistance()));
    }

    void build(const LA2AParams& p)
    {
        using namespace net;
        prm = p;
        auto n = [&](const char* s) { return c.node(s); };
        auto tri = [&](net::Triode t, int pl, int g, int k) {
            return static_cast<Triode*>(&c.device(std::make_unique<Triode>(t), Triode::ports(pl, g, k)));
        };
        const int bp = n("bp");
        supply = c.source(bp, GND, 0, p.bplus, true);

        // ---- input transformer -------------------------------------------------
        const auto& xi = p.in;
        const int b1 = n("b1"), mi = n("mi"), si = n("si"), sa = n("sa"), s7 = n("s7");
        input = c.source(b1, GND, p.rsrc + xi.rp);
        el["llp_in"] = c.inductor(b1, mi, xi.llp);
        coreIn = static_cast<JACore*>(&c.device(core(xi), JACore::ports(mi, GND)));
        c.idealTransformer(mi, GND, si, GND, xi.n);
        el["lls_in"] = c.inductor(si, sa, xi.lls);
        el["rs_in"] = c.resistor(sa, s7, xi.rs);
        el["cs_in"] = c.capacitor(s7, GND, xi.cs);
        el["r5"] = c.resistor(s7, GND, p.r5);

        // ---- attenuator: R6, R7, T4 photocell, Gain pot ----------------------------
        const int j = n("j"), att = n("att"), g1a = n("g1a");
        el["r6"] = c.resistor(s7, j, p.r6);
        el["r7"] = c.resistor(j, att, p.r7);
        el["r1a"] = c.resistor(att, g1a, p.gainPot);
        el["r1b"] = c.resistor(g1a, GND, p.gainPot);
        auto cell = std::make_unique<T4Cell>();
        cell->external = true;
        t4 = static_cast<T4Cell*>(&c.device(std::move(cell), T4Cell::cellPorts(att, GND)));
        el["rpk"] = c.resistor(j, GND, p.peakPot);   // the Peak Reduction pot's load on j
        jNode = j;

        // ---- V1A, V1B: 12AX7 voltage amplifier -----------------------------------------
        const int k1a = n("k1a"), p1a = n("p1a"), g1b = n("g1b"), k1b = n("k1b"), p1b = n("p1b");
        el["r9"] = c.resistor(bp, p1a, p.r9);
        el["r10"] = c.resistor(k1a, GND, p.r10);
        el["cm1a"] = c.capacitor(g1a, p1a, p.cmiller);
        v1a = tri(ax7(), p1a, g1a, k1a);
        el["c1"] = c.capacitor(p1a, g1b, p.c1);
        el["r12"] = c.resistor(g1b, GND, p.r12);
        el["r13"] = c.resistor(bp, p1b, p.r13);
        el["r14"] = c.resistor(k1b, GND, p.r14);
        el["cm1b"] = c.capacitor(g1b, p1b, p.cmiller);
        tri(ax7(), p1b, g1b, k1b);
        const int g2a = n("g2a"), k2a = n("k2a"), p2a = n("p2a"), sink = n("sink"), k2b = n("k2b"), g2b = n("g2b");
        el["c2"] = c.capacitor(p1b, g2a, p.c2);

        // ---- V2A cathode follower (grid leak bootstrapped to the bottom of R18),
        //      V2B current sink -------------------------------------------------------------
        el["r15"] = c.resistor(g2a, sink, p.r15);
        el["r17"] = c.resistor(bp, p2a, p.r17);
        el["r18"] = c.resistor(k2a, sink, p.r18);
        tri(bh7(), p2a, g2a, k2a);
        el["r19"] = c.resistor(g2b, GND, p.r19);
        el["r20"] = c.resistor(k2b, GND, p.r20);
        tri(bh7(), sink, g2b, k2b);
        el["c3"] = c.capacitor(p2a, g2b, p.c3);

        // ---- output transformer, feedback -----------------------------------------------
        const auto& xo = p.out;
        const int po = n("po"), mo = n("mo"), so = n("so"), sb = n("sb"), o = n("out");
        el["c5"] = c.capacitor(k2a, po, p.c5);
        el["rp_out"] = c.resistor(po, n("pl"), xo.rp);
        el["llp_out"] = c.inductor(n("pl"), mo, xo.llp);
        coreOut = static_cast<JACore*>(&c.device(core(xo), JACore::ports(mo, GND)));
        c.idealTransformer(mo, GND, so, GND, xo.n);
        el["lls_out"] = c.inductor(so, sb, xo.lls);
        el["rs_out"] = c.resistor(sb, o, xo.rs);
        el["cs_out"] = c.capacitor(o, GND, xo.cs);
        el["rload"] = c.resistor(o, GND, p.rload);
        // taken ahead of C5 and the transformer: their low-frequency poles inside
        // the loop make it motorboat
        const int fb = n("fb");
        el["cfb"] = c.capacitor(k2a, fb, p.cfb);
        el["rfb"] = c.resistor(fb, k1a, p.rfb);
        out = c.output(o);

        // ---- sidechain: Peak Reduction -> V3 pair -> V4 6AQ5 -> EL panel ----------------
        auto m = [&](const char* s) { return sc.node(s); };
        auto stri = [&](net::Triode t, int pl, int g, int k) {
            sc.device(std::make_unique<Triode>(t), Triode::ports(pl, g, k));
        };
        const int sbp = m("bp"), sj = m("j");
        scSupply = sc.source(sbp, GND, 0, p.bplus, true);
        scInput = sc.source(sj, GND, 0);
        const int g3a = m("g3a"), k3a = m("k3a"), tail = m("tail"), k3b = m("k3b"), g3b = m("g3b"), p3b = m("p3b");
        scel["r2a"] = sc.resistor(sj, g3a, p.peakPot);
        scel["r2b"] = sc.resistor(g3a, GND, p.peakPot);
        scel["r31"] = sc.resistor(k3a, tail, p.r31);
        scel["r32"] = sc.resistor(k3b, tail, p.r32);
        scel["rtail"] = sc.resistor(tail, GND, p.rtail);
        scel["rg3b"] = sc.resistor(g3b, GND, p.rg3b);
        scel["c8"] = sc.capacitor(g3b, GND, p.c8);
        scel["r33"] = sc.resistor(sbp, p3b, p.r33);
        stri(ax7(), sbp, g3a, k3a);
        stri(ax7(), p3b, g3b, k3b);
        const int g4 = m("g4"), k4 = m("k4"), p4 = m("p4"), g2 = m("g2"), elp = m("el");
        scel["c9"] = sc.capacitor(p3b, g4, p.c9);
        scel["r3"] = sc.resistor(g4, GND, p.r3);
        scel["r34"] = sc.resistor(sbp, p4, p.r34);
        scel["r35"] = sc.resistor(sbp, g2, p.r35);
        scel["c6"] = sc.capacitor(g2, GND, p.c6);
        scel["r36"] = sc.resistor(k4, GND, p.r36);
        scel["c10"] = sc.capacitor(k4, GND, p.c10);
        sc.device(std::make_unique<Pentode>(), Pentode::ports(p4, g4, g2, k4));
        scel["c11"] = sc.capacitor(p4, elp, p.c11);
        scel["cel"] = sc.capacitor(elp, GND, p.cel);
        scel["rel"] = sc.resistor(elp, GND, p.rel);
        elNode = elp;
        setGain(p.gain, p);
        setPeak(p.peak, p);
        c.rebuildIfDirty();
        sc.rebuildIfDirty();
    }
};

} // namespace cd
