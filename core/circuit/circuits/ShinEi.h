// ShinEi.h -- Shin-Ei Companion FY-2 "Fuzzmaster" as a netlist for the general
// engine. Mirrors sim/shinei/shinei.cir (the spec), itself a transcription of
// products/phys-fuzz/reference/shin-ei_fy2_fuzzmaster.pdf.
//
// Two collector-feedback silicon stages; the Fuzz pot pans between Q1's and
// Q2's collectors (2200p into the wiper, 3300p into lug 1, output from lug 3),
// then a passive mid scoop (1000p bridged by 10k/15k into 100n) into the 50k
// volume pot. Same pickup, battery and transistor model as the Fuzz Face, so
// the shared knob surface (Knobs.h) drives it too: knobsToShinEi().
#pragma once

#include "../Devices.h"
#include "../Knobs.h"

#include <map>
#include <string>

namespace cd {

struct ShinEiParams {
    // supply: battery EMF, internal resistance (dying battery), bulk cap
    double vcc = 9, rbat = 1, cbulk = 100e-9;
    // guitar pickup: source R, coil L, pickup+cable C
    double rsrc = 6e3, lpu = 2.5, ccab = 500e-12;
    // input coupling cap and its leakage
    double cin = 47e-9, rleakCin = 1e12;
    // Q1: collector-feedback bias resistor, its parallel cap, collector load
    double rb1 = 2.2e6, cfb1 = 1e-9, rc1 = 22e3;
    // Q2: interstage cap, bias resistor, collector load, starved supply (rsup || csup)
    double cc = 47e-9, rb2 = 1.2e6, rc2 = 47e3, rsup = 100e3, csup = 47e-9;
    // fuzz pot (0..1: wiper from the Q1 end to the Q2 end) and its feed caps
    double fuzz = 1, rfuzz = 50e3, cq1 = 2.2e-9, cq2 = 3.3e-9;
    // mid scoop
    double ctone = 1e-9, rtone1 = 10e3, rtone2 = 15e3, cmid = 100e-9;
    // volume pot (0..1), amp input
    double vol = 0.5, rvol = 50e3, rload = 1e6;
    // transistors
    double bf1 = 250, bf2 = 250, isat = 1e-14, vaf = 80;
    double rleak1 = 1e12, rleak2 = 1e12;
    double tempC = 27;
};

// Output scale that puts the healthy circuit's peak near -1 dBFS from a
// kPickupVolts input: the FY-2 is ~30 dB quieter than the Fuzz Face
// (sim/shinei/compare.py prints it).
inline constexpr double kOutputScaleShinEi = 8.1;

// The shared knob surface (Knobs.h, KnobIndex order) onto the FY-2. Bias Trim
// scales Q2's collector-feedback resistor (the FY-2 has no trimmer; 5.6k = the
// stock 1M2, higher = colder, as on the Fuzz Face). Junction leak spans a
// gentler range than the Fuzz Face's because it sits across the megohm bias
// resistors, where 30k would simply saturate the stage. Bypass Cap has no
// counterpart (no emitter bypass) and is ignored.
inline ShinEiParams knobsToShinEi(const double* v)
{
    ShinEiParams p;
    p.fuzz = v[kFuzz];
    p.vol = v[kVolume];
    p.vcc = v[kBattery];
    p.rbat = v[kBatteryRes];
    p.cbulk = v[kSupplyCap] * 1e-6;
    p.rb2 = 1.2e6 * v[kBias] / 5.6;
    p.bf1 = v[kQ1Gain];
    p.bf2 = v[kQ2Gain];
    p.rleak1 = p.rleak2 = leakToOhms(v[kJunctionLeak], 1e9, 3e5);
    p.rleakCin = leakToOhms(v[kCapLeak], 5e6, 1e4);
    p.tempC = v[kTemperature];
    return p;
}

struct ShinEi {
    // nodes exposed to the UI, in this order
    enum Node { VP, P1, GIN, B1, C1, B2, VA2, C2, FW, L1, F, X, S, OUT, N };

    net::Circuit c;
    int input = -1, out = -1, supply = -1;
    net::Bjt* q1 = nullptr;
    net::Bjt* q2 = nullptr;
    std::map<std::string, int> el;   // component name -> element index (for the bench)
    int nodes[N] {};

    static double pot(double frac, double r) { return std::max(frac * r, 1.0); }

    void build(const ShinEiParams& p)
    {
        using namespace net;
        auto n = [&](const char* s) { return c.node(s); };
        const int bat = n("bat"), vp = n("vp"), p1 = n("p1"), gin = n("gin"), b1 = n("b1"), c1 = n("c1");
        const int b2 = n("b2"), va2 = n("va2"), c2 = n("c2"), fw = n("fw"), l1 = n("l1"), f = n("f");
        const int x = n("x"), s = n("s"), o = n("out");
        const int list[N] = { vp, p1, gin, b1, c1, b2, va2, c2, fw, l1, f, x, s, o };
        for (int k = 0; k < N; ++k) nodes[k] = list[k];
        auto R = [&](E e, const char* name, int a, int b, double v) { ix[e] = el[name] = c.resistor(a, b, v); };
        auto C = [&](E e, const char* name, int a, int b, double v) { ix[e] = el[name] = c.capacitor(a, b, v); };

        // battery: ideal EMF behind its internal resistance (an element, so it can move)
        supply = c.source(bat, GND, 0, p.vcc, true);
        R(eRbat, "rbat", bat, vp, p.rbat);
        C(eCbulk, "cbulk", vp, GND, p.cbulk);
        input = c.source(p1, GND, p.rsrc);   // pickup EMF
        ix[eLpu] = el["lpu"] = c.inductor(p1, gin, p.lpu);
        C(eCcab, "ccab", gin, GND, p.ccab);

        C(eCin, "cin", gin, b1, p.cin);
        R(eRleakCin, "rleakcin", gin, b1, p.rleakCin);
        R(eRb1, "rb1", c1, b1, p.rb1);
        C(eCfb1, "cfb1", c1, b1, p.cfb1);
        R(eRleak1, "rleak1", c1, b1, p.rleak1);
        R(eRc1, "rc1", vp, c1, p.rc1);

        C(eCc, "cc", c1, b2, p.cc);
        R(eRb2, "rb2", c2, b2, p.rb2);
        R(eRleak2, "rleak2", c2, b2, p.rleak2);
        R(eRsup, "rsup", vp, va2, p.rsup);
        C(eCsup, "csup", vp, va2, p.csup);
        R(eRc2, "rc2", va2, c2, p.rc2);

        C(eCq1, "cq1", c1, fw, p.cq1);
        C(eCq2, "cq2", c2, l1, p.cq2);
        R(eRfza, "rfza", f, fw, pot(1 - p.fuzz, p.rfuzz));   // lug 3 -> wiper
        R(eRfzb, "rfzb", fw, l1, pot(p.fuzz, p.rfuzz));      // wiper -> lug 1

        C(eCtone, "ctone", f, x, p.ctone);
        R(eRtone1, "rtone1", f, s, p.rtone1);
        R(eRtone2, "rtone2", x, s, p.rtone2);
        C(eCmid, "cmid", s, GND, p.cmid);
        R(eRvtop, "rvtop", x, o, pot(1 - p.vol, p.rvol));
        R(eRvbot, "rvbot", o, GND, pot(p.vol, p.rvol));
        R(eRload, "rload", o, GND, p.rload);

        q1 = static_cast<Bjt*>(&c.device(std::make_unique<Bjt>(bjt::model(p.isat, p.bf1, p.vaf, p.tempC)), Bjt::ports(c1, b1, GND)));
        q2 = static_cast<Bjt*>(&c.device(std::make_unique<Bjt>(bjt::model(p.isat, p.bf2, p.vaf, p.tempC)), Bjt::ports(c2, b2, GND)));
        out = c.output(o);
    }

    // Move every value onto the running circuit without losing its state:
    // one matrix rebuild if anything linear changed, models and supply direct.
    // No lookups or allocation (audio-thread safe).
    void apply(const ShinEiParams& p)
    {
        auto set = [&](E e, double v) { c.setValueDeferred(ix[e], v); };
        set(eRbat, p.rbat); set(eCbulk, p.cbulk);
        set(eLpu, p.lpu); set(eCcab, p.ccab);
        set(eCin, p.cin); set(eRleakCin, p.rleakCin);
        set(eRb1, p.rb1); set(eCfb1, p.cfb1); set(eRleak1, p.rleak1); set(eRc1, p.rc1);
        set(eCc, p.cc); set(eRb2, p.rb2); set(eRleak2, p.rleak2);
        set(eRsup, p.rsup); set(eCsup, p.csup); set(eRc2, p.rc2);
        set(eCq1, p.cq1); set(eCq2, p.cq2);
        set(eRfza, pot(1 - p.fuzz, p.rfuzz)); set(eRfzb, pot(p.fuzz, p.rfuzz));
        set(eCtone, p.ctone); set(eRtone1, p.rtone1); set(eRtone2, p.rtone2); set(eCmid, p.cmid);
        set(eRvtop, pot(1 - p.vol, p.rvol)); set(eRvbot, pot(p.vol, p.rvol)); set(eRload, p.rload);
        c.setInput(supply, p.vcc);
        q1->m = bjt::model(p.isat, p.bf1, p.vaf, p.tempC);
        q2->m = bjt::model(p.isat, p.bf2, p.vaf, p.tempC);
        c.rebuildIfDirty();
    }

    // one sample at the oversampled rate; vin = pickup EMF in volts, returns v(out)
    double process(double vin)
    {
        c.setInput(input, vin);
        c.process();
        return c.out(out);
    }

    double node(Node k) const { return c.x(nodes[k]); }

private:
    enum E { eRbat, eCbulk, eLpu, eCcab, eCin, eRleakCin, eRb1, eCfb1, eRleak1, eRc1, eCc, eRb2, eRleak2, eRsup, eCsup,
             eRc2, eCq1, eCq2, eRfza, eRfzb, eCtone, eRtone1, eRtone2, eCmid, eRvtop, eRvbot, eRload, kElems };
    int ix[kElems] {};
};

} // namespace cd
