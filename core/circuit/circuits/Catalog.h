// Catalog.h -- the netlist circuits the workbench can run, each with every
// component exposed as a knob. Plain C++ (no JUCE): Circuit Bench builds its
// controls from controls(), feeds values to apply() at control rate and audio
// through process().
//
// apply() maps each knob onto the running circuit without losing its state:
// resistors/capacitors/inductors and the transformer ratio are batched into
// one matrix rebuild; supply voltage is a source input; core and tube
// parameters are written straight into the devices.
#pragma once

#include "../Knobs.h"
#include "LA2A.h"
#include "../../acoustic/CabModel.h"
#include "MicTransformer.h"
#include "SEOutput.h"
#include "ShinEi.h"
#include "TubePre.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace cd::catalog {

enum class Target { Element, Ratio, Supply, Core, Tube, InputLevel, Output, Custom };

struct Control {
    Knob knob;          // id, name, range (display units), default, unit
    Target target;
    const char* key;    // element name / device field
    double scale;       // display units -> SI
    const char* group;
};

struct Probe {
    const char* label;
    int node;
    int circuit = 0;   // for circuits built from more than one net::Circuit
};

inline double dbuPeakVolts(double dbu) { return 0.775 * std::sqrt(2.0) * std::pow(10.0, dbu / 20.0); }

class BenchCircuit {
public:
    virtual ~BenchCircuit() = default;
    const std::vector<Control>& controls() const { return ctl; }
    const std::vector<Probe>& probes() const { return prb; }
    net::JACore* core() const { return coreDev; }
    net::Circuit& circuit() { return *c; }

    virtual void prepare(double sampleRate) { c->prepare(sampleRate); }
    virtual void warmStart() { c->warmStart(); }
    virtual long failureCount() const { return c->failures; }
    int iterations() const { return c->lastIterations; }
    virtual double probeVoltage(size_t k) const { return c->x(prb[k].node); }
    // gain reduction in dB for circuits that have it (compressors), else NaN
    virtual double gainReductionDb() const { return NAN; }
    // the speaker cab model, for circuits that are one (views read it on the UI thread)
    virtual const acoustic::Cab* cabModel() const { return nullptr; }

    void apply(const double* v)
    {
        for (size_t k = 0; k < ctl.size(); ++k) applyOne(ctl[k], v[k]);
        rebuild();
    }

    // full-scale in -> full-scale out, normalised by the circuit's nominal gain
    // so the default settings play at roughly unity
    virtual double process(double x)
    {
        c->setInput(input, x * inVolts);
        c->process();
        return c->out(out) / (nominalGain * inVoltsDefault) * outGain;
    }

protected:
    net::Circuit* c = nullptr;
    std::map<std::string, int>* el = nullptr;
    net::JACore* coreDev = nullptr;
    net::Triode* tubeDev = nullptr;
    int input = -1, out = -1, supply = -1;
    double nominalGain = 1, inVolts = 1, inVoltsDefault = 1, outGain = 1;
    std::vector<Control> ctl;
    std::vector<Probe> prb;

    virtual void applyOne(const Control& q, double raw)
    {
        const double x = raw * q.scale;
        switch (q.target) {
        case Target::Element: c->setValueDeferred((*el)[q.key], x); break;
        case Target::Ratio: c->setRatioDeferred(0, x); break;
        case Target::Supply: c->setInput(supply, x); break;
        case Target::Core: setField(coreFields(), q.key, x); break;
        case Target::Tube: setField(tubeFields(), q.key, x); break;
        case Target::InputLevel: inVolts = dbuPeakVolts(raw); break;
        case Target::Output: outGain = std::pow(10.0, raw / 20.0); break;
        case Target::Custom: break;
        }
    }
    virtual void rebuild() { c->rebuildIfDirty(); }

    struct Field { const char* key; double* p; };
    std::vector<Field> coreFields() const
    {
        if (!coreDev) return {};
        auto* k = coreDev;
        return { { "np", &k->np }, { "ac", &k->ac }, { "le", &k->le }, { "lg", &k->lg }, { "ms", &k->ms },
                 { "a", &k->a }, { "alpha", &k->alpha }, { "k", &k->k }, { "c", &k->c } };
    }
    std::vector<Field> tubeFields() const
    {
        if (!tubeDev) return {};
        auto* t = tubeDev;
        return { { "mu", &t->mu }, { "ex", &t->ex }, { "kg1", &t->kg1 }, { "kp", &t->kp }, { "kvb", &t->kvb }, { "kgc", &t->kgc } };
    }
    static void setField(const std::vector<Field>& fs, const char* key, double x)
    {
        for (auto& f : fs)
            if (!std::strcmp(f.key, key)) { *f.p = x; return; }
    }

    // shared control blocks ----------------------------------------------------
    static void ioControls(std::vector<Control>& v, double defDbu, double loDbu, double hiDbu)
    {
        v.push_back({ { "level", "Source level", loDbu, hiDbu, loDbu, defDbu, "dBu pk", 1, false }, Target::InputLevel, "", 1, "Input / output" });
        v.push_back({ { "output", "Output", -36, 24, -36, 0, "dB", 1, false }, Target::Output, "", 1, "Input / output" });
    }
    static void coreControls(std::vector<Control>& v, const net::JACore& d)
    {
        const char* g = "Core";
        v.push_back({ { "np", "Primary turns", 100, 8000, 1000, d.np, "", 0, false }, Target::Core, "np", 1, g });
        v.push_back({ { "ac", "Core area", 0.1, 10, 1, d.ac * 1e4, "cm2", 2, false }, Target::Core, "ac", 1e-4, g });
        v.push_back({ { "le", "Path length", 1, 30, 5, d.le * 100, "cm", 1, false }, Target::Core, "le", 1e-2, g });
        v.push_back({ { "lg", "Air gap", 0, 0.5, 0, d.lg * 1e3, "mm", 3, false }, Target::Core, "lg", 1e-3, g });
        v.push_back({ { "ms", "Saturation Ms", 0.1, 2, 0.1, d.ms * 1e-6, "MA/m", 2, false }, Target::Core, "ms", 1e6, g });
        v.push_back({ { "a", "JA shape a", 1, 500, 30, d.a, "A/m", 1, false }, Target::Core, "a", 1, g });
        v.push_back({ { "alpha", "JA coupling", 0, 30, 0, d.alpha * 1e5, "x1e-5", 2, false }, Target::Core, "alpha", 1e-5, g });
        v.push_back({ { "k", "JA pinning k", 0.5, 200, 10, d.k, "A/m", 1, false }, Target::Core, "k", 1, g });
        v.push_back({ { "c", "JA reversibility", 0.01, 0.99, 0.01, d.c, "", 2, false }, Target::Core, "c", 1, g });
    }
    static void tubeControls(std::vector<Control>& v, const net::Triode& t, const char* g)
    {
        v.push_back({ { "mu", "mu", 5, 120, 5, t.mu, "", 1, false }, Target::Tube, "mu", 1, g });
        v.push_back({ { "ex", "Exponent", 1, 2, 1, t.ex, "", 2, false }, Target::Tube, "ex", 1, g });
        v.push_back({ { "kg1", "kg1", 100, 5000, 1000, t.kg1, "", 0, false }, Target::Tube, "kg1", 1, g });
        v.push_back({ { "kp", "kp", 20, 1500, 200, t.kp, "", 0, false }, Target::Tube, "kp", 1, g });
        v.push_back({ { "kvb", "kvb", 10, 3000, 300, t.kvb, "", 0, false }, Target::Tube, "kvb", 1, g });
        v.push_back({ { "kgc", "Grid current", 0, 20, 0, t.kgc * 1e4, "x1e-4", 2, false }, Target::Tube, "kgc", 1e-4, g });
    }
    static Control element(const char* key, const char* name, double lo, double hi, double centre, double def,
                           const char* unit, double scale, int dec, const char* g)
    {
        return { { key, name, lo, hi, centre, def, unit, dec, false }, Target::Element, key, scale, g };
    }
};

// ---- mic transformer ---------------------------------------------------------
class MicTransformerBench : public BenchCircuit {
public:
    MicTransformerBench()
    {
        TransformerParams p;
        ckt.build(p);
        c = &ckt.c; el = &ckt.el; coreDev = ckt.core; input = ckt.input; out = ckt.out;
        nominalGain = 8.7;
        inVolts = inVoltsDefault = dbuPeakVolts(0);
        ioControls(ctl, 0, -40, 20);
        const char* g = "Transformer";
        ctl.push_back({ { "n", "Turns ratio", 1, 20, 1, p.n, ":1", 1, false }, Target::Ratio, "n", 1, g });
        ctl.push_back(element("ll1", "Primary leakage", 0.01, 5, 0.2, p.ll1 * 1e3, "mH", 1e-3, 3, g));
        ctl.push_back(element("ll2", "Secondary leakage", 1, 500, 20, p.ll2 * 1e3, "mH", 1e-3, 1, g));
        ctl.push_back(element("rs", "Secondary DCR", 100, 10000, 2500, p.rs, "Ohm", 1, 0, g));
        ctl.push_back(element("cs", "Secondary C", 10, 2000, 100, p.cs * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("rload", "Load", 5, 1000, 150, p.rload * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("rz", "Zobel R", 1, 1000, 100, p.rz * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("cz", "Zobel C", 10, 10000, 100, p.cz * 1e12, "pF", 1e-12, 0, g));
        coreControls(ctl, *ckt.core);
        auto node = [&](const char* s) { return ckt.c.node(s); };
        prb = { { "primary", node("m") }, { "secondary", node("sp") }, { "output", node("s") } };
    }
private:
    MicTransformer ckt;
};

// ---- tube mic pre --------------------------------------------------------------
class TubePreBench : public BenchCircuit {
public:
    TubePreBench()
    {
        TubePreParams p;
        ckt.build(p);
        c = &ckt.c; el = &ckt.el; coreDev = ckt.core; tubeDev = ckt.tube;
        input = ckt.input; out = ckt.out; supply = ckt.supply;
        nominalGain = 510;
        inVolts = inVoltsDefault = dbuPeakVolts(-40);
        ioControls(ctl, -40, -70, 0);
        const auto& x = p.xfmr;
        const char* g = "Input transformer";
        ctl.push_back({ { "n", "Turns ratio", 1, 20, 1, x.n, ":1", 1, false }, Target::Ratio, "n", 1, g });
        ctl.push_back(element("ll1", "Primary leakage", 0.01, 5, 0.2, x.ll1 * 1e3, "mH", 1e-3, 3, g));
        ctl.push_back(element("ll2", "Secondary leakage", 1, 500, 20, x.ll2 * 1e3, "mH", 1e-3, 1, g));
        ctl.push_back(element("rs", "Secondary DCR", 100, 10000, 2500, x.rs, "Ohm", 1, 0, g));
        ctl.push_back(element("cs", "Secondary C", 10, 2000, 100, x.cs * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("rload", "Secondary load", 5, 1000, 150, x.rload * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("rz", "Zobel R", 1, 1000, 100, x.rz * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("cz", "Zobel C", 10, 10000, 100, x.cz * 1e12, "pF", 1e-12, 0, g));
        coreControls(ctl, *ckt.core);
        g = "12AX7 stage";
        ctl.push_back({ { "bplus", "B+", 50, 400, 50, p.bplus, "V", 0, false }, Target::Supply, "", 1, g });
        ctl.push_back(element("ra", "Plate R", 10, 1000, 100, p.ra * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("rk", "Cathode R", 0.1, 20, 1.5, p.rk * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("ck", "Cathode bypass", 0.1, 1000, 22, p.ck * 1e6, "uF", 1e-6, 1, g));
        ctl.push_back(element("rg", "Grid stopper", 0.01, 100, 2.2, p.rg * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("cgp", "Miller C (g-p)", 0.1, 50, 1.7, p.cgp * 1e12, "pF", 1e-12, 2, g));
        ctl.push_back(element("cc", "Coupling C", 1, 2000, 100, p.cc * 1e9, "nF", 1e-9, 1, g));
        ctl.push_back(element("rout", "Output load", 10, 10000, 1000, p.rout * 1e-3, "kOhm", 1e3, 0, g));
        tubeControls(ctl, *ckt.tube, "12AX7 curve (Koren)");
        auto node = [&](const char* s) { return ckt.c.node(s); };
        prb = { { "primary", node("m") }, { "grid", node("grid") }, { "cathode", node("cath") },
                { "plate", node("plate") }, { "output", node("out") } };
    }
private:
    TubePre ckt;
};

// ---- single-ended output stage (Iron's circuit) ----------------------------------
class SEOutputBench : public BenchCircuit {
public:
    SEOutputBench()
    {
        SEOutputParams p;
        ckt.build(p);
        c = &ckt.c; el = &ckt.el; coreDev = ckt.core; tubeDev = ckt.tube;
        input = ckt.input; out = ckt.out; supply = ckt.supply;
        nominalGain = 4.2;
        inVolts = inVoltsDefault = dbuPeakVolts(-2.2);   // 1 V peak
        ioControls(ctl, -2.2, -30, 20);
        const char* g = "12AU7 stage";
        ctl.push_back({ { "bplus", "B+", 50, 400, 50, p.bplus, "V", 0, false }, Target::Supply, "", 1, g });
        ctl.push_back(element("rk", "Cathode R", 0.1, 10, 0.82, p.rk * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("ck", "Cathode bypass", 0.1, 1000, 100, p.ck * 1e6, "uF", 1e-6, 1, g));
        ctl.push_back(element("rgk", "Grid leak", 10, 10000, 1000, p.rgk * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(element("cgp", "Miller C (g-p)", 0.1, 50, 1.5, p.cgp * 1e12, "pF", 1e-12, 2, g));
        tubeControls(ctl, *ckt.tube, "12AU7 curve (Koren)");
        g = "Output transformer";
        ctl.push_back({ { "n", "Turns ratio", 0.02, 1, 0.25, p.n, ":1", 3, false }, Target::Ratio, "n", 1, g });
        ctl.push_back(element("rprim", "Primary DCR", 10, 5000, 800, p.rprim, "Ohm", 1, 0, g));
        ctl.push_back(element("llp", "Primary leakage", 0.5, 200, 10, p.llp * 1e3, "mH", 1e-3, 1, g));
        ctl.push_back(element("rsec", "Secondary DCR", 1, 500, 40, p.rsec, "Ohm", 1, 1, g));
        ctl.push_back(element("lls", "Secondary leakage", 0.01, 20, 0.6, p.lls * 1e3, "mH", 1e-3, 2, g));
        ctl.push_back(element("csec", "Secondary C", 10, 5000, 200, p.csec * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("rload", "Load", 0.1, 100, 10, p.rload * 1e-3, "kOhm", 1e3, 2, g));
        coreControls(ctl, *ckt.core);
        auto node = [&](const char* s) { return ckt.c.node(s); };
        prb = { { "grid", node("grid") }, { "cathode", node("cath") }, { "plate", node("plate") },
                { "supply", node("bp") }, { "primary", node("m") }, { "output", node("out") } };
    }
private:
    SEOutput ckt;
};

// ---- LA-2A leveling amplifier ----------------------------------------------------------
// Two circuits (audio path + sidechain, LA2A.h). Knobs keyed "sc:<name>" are
// sidechain components; "t4:<field>" the T4 cell; front-panel pots, B+ and the
// two transformer ratios are handled here too.
class LA2ABench : public BenchCircuit {
public:
    LA2ABench()
    {
        p.gain = 0.5;
        ckt.build(p);
        c = &ckt.c; el = &ckt.el; coreDev = ckt.coreOut; input = ckt.input; out = ckt.out; supply = ckt.supply;
        nominalGain = 51.3;   // Gain 50%: 34.2 dB with the cell dark
        inVolts = inVoltsDefault = dbuPeakVolts(-10);
        ioControls(ctl, -10, -40, 20);
        const char* g = "Front panel";
        ctl.push_back(custom("gain", "Gain", 0, 100, 50, p.gain * 100, "%", 0, g));
        ctl.push_back(custom("peak", "Peak Reduction", 0, 100, 50, p.peak * 100, "%", 0, g));
        ctl.push_back(custom("bplus", "B+", 100, 350, 100, p.bplus, "V", 0, g));
        g = "Attenuator";
        ctl.push_back(element("r5", "R5 (secondary load)", 5, 500, 68, p.r5 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("r6", "R6 (series)", 5, 500, 68, p.r6 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("r7", "R7 (to cell)", 0.1, 50, 2.7, p.r7 * 1e-3, "kOhm", 1e3, 2, g));
        g = "T4 cell";
        const auto& t = *ckt.t4;
        ctl.push_back(custom("t4:rfull", "R at full light", 50, 5000, 400, 1 / t.gFull, "Ohm", 0, g));
        ctl.push_back(custom("t4:rdark", "Dark R", 0.1, 50, 5, t.rDark * 1e-6, "MOhm", 2, g));
        ctl.push_back(custom("t4:vref", "EL full-light V", 20, 500, 150, t.vRef, "V", 0, g));
        ctl.push_back(custom("t4:vthresh", "EL threshold", 0, 50, 8, t.vThresh, "V", 1, g));
        ctl.push_back(custom("t4:taulight", "Panel lag", 0.1, 20, 1, t.tauLight * 1e3, "ms", 2, g));
        ctl.push_back(custom("t4:gamma", "Photo exponent", 0.3, 1.5, 0.8, t.gamma, "", 2, g));
        ctl.push_back(custom("t4:fast", "Fast share", 0, 1, 0, t.fastShare, "", 2, g));
        ctl.push_back(custom("t4:fastoff", "Fast release", 3, 500, 30, t.tauFastOff * 1e3, "ms", 1, g));
        ctl.push_back(custom("t4:slowoff", "Slow release", 0.02, 3, 0.18, t.tauSlowOff, "s", 2, g));
        ctl.push_back(custom("t4:mem", "Memory stretch", 0, 30, 6, t.memStretch, "x", 1, g));
        ctl.push_back(custom("t4:memoff", "Memory fade", 0.5, 60, 12, t.tauMemOff, "s", 1, g));
        g = "Sidechain";
        ctl.push_back(element("sc:rtail", "V3 tail R", 0.2, 20, 2.2, p.rtail * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("sc:r33", "V3B plate R33", 22, 1000, 220, p.r33 * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(element("sc:c9", "C9 coupling", 1, 200, 20, p.c9 * 1e9, "nF", 1e-9, 1, g));
        ctl.push_back(element("sc:r34", "V4 plate R34", 2.2, 100, 22, p.r34 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("sc:r35", "V4 screen R35", 10, 1000, 220, p.r35 * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(element("sc:r36", "V4 cathode R36", 0.1, 10, 1, p.r36 * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("sc:c11", "C11 to panel", 5, 1000, 100, p.c11 * 1e9, "nF", 1e-9, 0, g));
        ctl.push_back(element("sc:cel", "EL panel C", 1, 100, 10, p.cel * 1e9, "nF", 1e-9, 1, g));
        ctl.push_back(element("sc:rel", "EL panel leak", 22, 5000, 470, p.rel * 1e-3, "kOhm", 1e3, 0, g));
        g = "Amplifier";
        ctl.push_back(element("rfb", "Feedback R", 47, 5000, 470, p.rfb * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(element("r10", "V1A cathode R10", 0.2, 10, 1.5, p.r10 * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("r14", "V1B cathode R14", 0.2, 10, 2.7, p.r14 * 1e-3, "kOhm", 1e3, 2, g));
        ctl.push_back(element("c3", "C3 (White CF)", 1, 1000, 100, p.c3 * 1e9, "nF", 1e-9, 0, g));
        ctl.push_back(element("c5", "C5 output cap", 0.5, 100, 10, p.c5 * 1e6, "uF", 1e-6, 1, g));
        g = "Transformers";
        ctl.push_back(custom("ratio:in", "Input step-up", 1, 10, 4, p.in.n, ":1", 1, g));
        ctl.push_back(custom("ratio:out", "Output step-down", 1, 10, 3, 1 / p.out.n, ":1", 1, g));
        coreControls(ctl, *ckt.coreOut);
        for (auto& q : ctl)
            if (!std::strcmp(q.group, "Core")) q.group = "Output transformer core";
        auto node = [&](const char* s) { return ckt.c.node(s); };
        prb = { { "j (to sidechain)", node("j") }, { "att (cell)", node("att") }, { "V1A plate", node("p1a") },
                { "V2A cathode", node("k2a") }, { "output", node("out") },
                { "V4 plate", ckt.sc.node("p4"), 1 }, { "EL panel", ckt.elNode, 1 } };
    }

    void prepare(double sampleRate) override { ckt.prepare(sampleRate, 4); }   // sidechain at 1/4 rate
    void warmStart() override { ckt.c.warmStart(); ckt.sc.warmStart(); }
    long failureCount() const override { return ckt.c.failures + ckt.sc.failures; }
    double probeVoltage(size_t k) const override
    {
        return prb[k].circuit ? ckt.sc.x(prb[k].node) : ckt.c.x(prb[k].node);
    }
    double gainReductionDb() const override { return ckt.gainReductionDb(); }
    double process(double x) override
    {
        ckt.process(x * inVolts);
        return ckt.output() / (nominalGain * inVoltsDefault) * outGain;
    }

protected:
    void applyOne(const Control& q, double raw) override
    {
        const std::string key = q.key;
        const double x = raw * q.scale;
        auto& t = *ckt.t4;
        if (q.target == Target::Element && key.rfind("sc:", 0) == 0) { ckt.sc.setValueDeferred(ckt.scel[key.substr(3)], x); return; }
        if (q.target != Target::Custom) { BenchCircuit::applyOne(q, raw); return; }
        if (key == "gain") ckt.setGain(raw / 100, p);
        else if (key == "peak") ckt.setPeak(raw / 100, p);
        else if (key == "bplus") ckt.setSupply(raw);
        else if (key == "ratio:in") ckt.c.setRatioDeferred(0, raw);
        else if (key == "ratio:out") ckt.c.setRatioDeferred(1, 1 / raw);
        else if (key == "t4:rfull") t.gFull = 1 / raw;
        else if (key == "t4:rdark") t.rDark = raw * 1e6;
        else if (key == "t4:vref") t.vRef = raw;
        else if (key == "t4:vthresh") t.vThresh = raw;
        else if (key == "t4:taulight") t.tauLight = raw * 1e-3;
        else if (key == "t4:gamma") t.gamma = raw;
        else if (key == "t4:fast") t.fastShare = raw;
        else if (key == "t4:fastoff") t.tauFastOff = raw * 1e-3;
        else if (key == "t4:slowoff") t.tauSlowOff = raw;
        else if (key == "t4:mem") t.memStretch = raw;
        else if (key == "t4:memoff") t.tauMemOff = raw;
    }
    void rebuild() override
    {
        ckt.c.rebuildIfDirty();
        ckt.sc.rebuildIfDirty();
    }

private:
    LA2AParams p;
    LA2A ckt;

    static Control custom(const char* key, const char* name, double lo, double hi, double centre, double def,
                          const char* unit, int dec, const char* g)
    {
        return { { key, name, lo, hi, centre, def, unit, dec, false }, Target::Custom, key, 1, g };
    }
};

// ---- Shin-Ei FY-2 fuzz --------------------------------------------------------
// Every component of the FY-2 netlist (ShinEi.h); the pots, transistor gains,
// leaks and temperature go through the same apply() the plugin uses.
class ShinEiBench : public BenchCircuit {
public:
    ShinEiBench()
    {
        ckt.build(p);
        c = &ckt.c; el = &ckt.el; input = ckt.input; out = ckt.out; supply = ckt.supply;
        ckt.c.maxIterations = 8;
        nominalGain = 0.73;   // 150 mV pickup peak -> 110 mV out at the defaults
        inVolts = inVoltsDefault = kPickupVolts;
        ioControls(ctl, -17.3, -50, 10);   // 150 mV pk = -17.3 dBu
        const char* g = "Pots";
        ctl.push_back(custom("fuzz", "Fuzz (Q1 <-> Q2)", 0, 1, 0, p.fuzz, "", 2, g));
        ctl.push_back(custom("vol", "Volume", 0, 1, 0, p.vol, "", 2, g));
        g = "Supply";
        ctl.push_back({ { "vcc", "Battery", 0.5, 12, 0.5, p.vcc, "V", 2, false }, Target::Supply, "", 1, g });
        ctl.push_back(element("rbat", "Battery sag", 1, 5000, 200, p.rbat, "Ohm", 1, 0, g));
        ctl.push_back(element("cbulk", "Supply cap", 0.01, 470, 1, p.cbulk * 1e6, "uF", 1e-6, 2, g));
        g = "Q1 stage";
        ctl.push_back(element("cin", "Input cap", 1, 470, 47, p.cin * 1e9, "nF", 1e-9, 0, g));
        ctl.push_back(custom("rleakcin", "Input cap leak", 0, 1, 0, 0, "", 2, g));
        ctl.push_back(element("rb1", "Bias R (c-b)", 0.1, 10, 2.2, p.rb1 * 1e-6, "MOhm", 1e6, 2, g));
        ctl.push_back(element("cfb1", "Feedback C (c-b)", 10, 10000, 1000, p.cfb1 * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("rc1", "Collector R", 1, 220, 22, p.rc1 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(custom("bf1", "Q1 hFE", 5, 600, 100, p.bf1, "", 0, g));
        ctl.push_back(custom("leak1", "Q1 junction leak", 0, 1, 0, 0, "", 2, g));
        g = "Q2 stage";
        ctl.push_back(element("cc", "Coupling cap", 1, 470, 47, p.cc * 1e9, "nF", 1e-9, 0, g));
        ctl.push_back(element("rb2", "Bias R (c-b)", 0.1, 10, 1.2, p.rb2 * 1e-6, "MOhm", 1e6, 2, g));
        ctl.push_back(element("rc2", "Collector R", 1, 470, 47, p.rc2 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("rsup", "Supply R", 1, 1000, 100, p.rsup * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(element("csup", "Supply bypass", 1, 1000, 47, p.csup * 1e9, "nF", 1e-9, 0, g));
        ctl.push_back(custom("bf2", "Q2 hFE", 5, 600, 100, p.bf2, "", 0, g));
        ctl.push_back(custom("leak2", "Q2 junction leak", 0, 1, 0, 0, "", 2, g));
        g = "Fuzz pot";
        ctl.push_back(element("cq1", "Q1 -> wiper", 100, 100000, 2200, p.cq1 * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("cq2", "Q2 -> lug 1", 100, 100000, 3300, p.cq2 * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(custom("rfuzz", "Pot", 1, 500, 50, p.rfuzz * 1e-3, "kOhm", 0, g));
        g = "Mid scoop";
        ctl.push_back(element("ctone", "Series C", 100, 100000, 1000, p.ctone * 1e12, "pF", 1e-12, 0, g));
        ctl.push_back(element("rtone1", "Bridge R (in)", 0.1, 100, 10, p.rtone1 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("rtone2", "Bridge R (out)", 0.1, 100, 15, p.rtone2 * 1e-3, "kOhm", 1e3, 1, g));
        ctl.push_back(element("cmid", "Shunt C", 1, 10000, 100, p.cmid * 1e9, "nF", 1e-9, 0, g));
        g = "Output";
        ctl.push_back(custom("rvol", "Volume pot", 1, 500, 50, p.rvol * 1e-3, "kOhm", 0, g));
        ctl.push_back(element("rload", "Amp input", 10, 10000, 1000, p.rload * 1e-3, "kOhm", 1e3, 0, g));
        ctl.push_back(custom("temp", "Temperature", -40, 120, -40, p.tempC, "C", 0, "Transistors"));
        using S = ShinEi;
        prb = { { "Q1 base", ckt.nodes[S::B1] }, { "Q1 collector", ckt.nodes[S::C1] }, { "Q2 base", ckt.nodes[S::B2] },
                { "Q2 supply", ckt.nodes[S::VA2] }, { "Q2 collector", ckt.nodes[S::C2] }, { "fuzz wiper", ckt.nodes[S::FW] },
                { "scoop in", ckt.nodes[S::F] }, { "scoop out", ckt.nodes[S::X] }, { "output", ckt.nodes[S::OUT] } };
    }

protected:
    void applyOne(const Control& q, double raw) override
    {
        if (q.target != Target::Custom) { BenchCircuit::applyOne(q, raw); return; }
        const std::string key = q.key;
        if (key == "fuzz") p.fuzz = raw;
        else if (key == "vol") p.vol = raw;
        else if (key == "rfuzz") p.rfuzz = raw * 1e3;
        else if (key == "rvol") p.rvol = raw * 1e3;
        else if (key == "bf1") p.bf1 = raw;
        else if (key == "bf2") p.bf2 = raw;
        else if (key == "temp") p.tempC = raw;
        else if (key == "leak1") p.rleak1 = leakToOhms(raw, 1e9, 3e5);
        else if (key == "leak2") p.rleak2 = leakToOhms(raw, 1e9, 3e5);
        else if (key == "rleakcin") p.rleakCin = leakToOhms(raw, 5e6, 1e4);
    }
    void rebuild() override
    {
        // pots, models and leaks through the netlist's own apply (element knobs
        // were already set deferred by the base class; apply() rebuilds once)
        ckt.c.setValueDeferred(ckt.el["rfza"], ShinEi::pot(1 - p.fuzz, p.rfuzz));
        ckt.c.setValueDeferred(ckt.el["rfzb"], ShinEi::pot(p.fuzz, p.rfuzz));
        ckt.c.setValueDeferred(ckt.el["rvtop"], ShinEi::pot(1 - p.vol, p.rvol));
        ckt.c.setValueDeferred(ckt.el["rvbot"], ShinEi::pot(p.vol, p.rvol));
        ckt.c.setValueDeferred(ckt.el["rleak1"], p.rleak1);
        ckt.c.setValueDeferred(ckt.el["rleak2"], p.rleak2);
        ckt.c.setValueDeferred(ckt.el["rleakcin"], p.rleakCin);
        ckt.q1->m = bjt::model(p.isat, p.bf1, p.vaf, p.tempC);
        ckt.q2->m = bjt::model(p.isat, p.bf2, p.vaf, p.tempC);
        ckt.c.rebuildIfDirty();
    }

private:
    ShinEiParams p;
    ShinEi ckt;

    static Control custom(const char* key, const char* name, double lo, double hi, double centre, double def,
                          const char* unit, int dec, const char* g)
    {
        return { { key, name, lo, hi, centre, def, unit, dec, false }, Target::Custom, key, 1, g };
    }
};

// ---- speaker cab (docs/briefs/2026-09-26-speaker-cab.md) ------------------------------
// The driver + box circuit per sample, convolved with the physics IR a worker
// thread rebuilds when a knob moves (acoustic/CabModel.h). Defaults: the
// Eminence Legend 1258 as calibrated in sim/speaker/, 50 l closed box, a
// cardioid 2 cm capsule 2.5 cm from the dust cap. Speaker size rescales the
// cone (the moving mass follows); the back opening turns it into an open back.
class CabBench : public BenchCircuit {
public:
    CabBench()
    {
        using namespace acoustic;
        const auto p = legend1258();
        auto add = [&](int idx, const char* key, const char* name, double lo, double hi, double centre, double scale,
                       const char* unit, int dec, const char* g) {
            ctl.push_back({ { key, name, lo, hi, centre, p[(size_t) idx] / scale, unit, dec, false }, Target::Custom, key, scale, g });
            index.push_back(idx);
        };
        ctl.push_back({ { "amp", "Amp peak (full scale)", 1, 60, 10, 20, "V", 1, false }, Target::Custom, "amp", 1, "Input / output" });
        index.push_back(-1);
        ctl.push_back({ { "output", "Output", -36, 24, -36, 0, "dB", 1, false }, Target::Output, "", 1, "Input / output" });
        index.push_back(-1);
        const char* g = "Microphone";
        add(kMicOffset, "micoff", "Across the cone", 0, 15, 5, 1e-2, "cm", 1, g);
        add(kMicDistance, "micdist", "Distance", 0.5, 60, 5, 1e-2, "cm", 1, g);
        add(kMicAngle, "micang", "Angle (towards centre)", -60, 60, -60, M_PI / 180, "deg", 0, g);
        add(kMicCapsule, "miccap", "Capsule diameter", 3, 40, 15, 1e-3, "mm", 0, g);
        add(kMicModel, "micmodel", "Measured dynamic (1) / ideal (0)", 0, 1, 0, 1, "", 0, g);
        add(kMicPattern, "micpat", "Ideal pattern (1 omni, 0 fig-8)", 0, 1, 0, 1, "", 2, g);
        add(kMicFace, "micface", "Mic face (reflects; 0 = none)", 0, 60, 30, 1e-3, "mm", 0, g);
        g = "Cone";
        add(kConeRadius, "size", "Speaker size (nominal)", 6, 15, 10, 0.01058, "in", 1, g);   // radiating radius ~ 0.83 x nominal / 2
        add(kYoungs, "E", "Paper stiffness", 0.5, 10, 3, 1e9, "GPa", 2, g);
        add(kDensity, "rho", "Paper density", 200, 1000, 450, 1, "kg/m3", 0, g);
        add(kThickness, "h", "Thickness (edge)", 0.15, 1.0, 0.35, 1e-3, "mm", 3, g);
        add(kTaper, "taper", "Thicker at the neck", 1, 3, 1, 1, "x", 2, g);
        add(kRibs, "ribs", "Ribs (radial bend stiff.)", 1, 20, 3, 1, "x", 2, g);
        add(kAniso, "aniso", "Hoop / radial stiffness", 0.2, 1, 0.2, 1, "", 2, g);
        add(kLoss, "eta", "Paper loss", 0.005, 0.3, 0.05, 1, "", 3, g);
        add(kDepth, "depth", "Depth", 2, 8, 2, 1e-2, "cm", 1, g);
        add(kCurve, "curve", "Curvilinear", 0, 1.5, 0, 1, "", 2, g);
        add(kDustCap, "dcr", "Dust cap radius", 2.5, 7, 2.5, 1e-2, "cm", 1, g);
        add(kCapMass, "dcm", "Dust cap mass", 0.1, 3, 0.7, 1e-3, "g", 2, g);
        g = "Surround";
        add(kSurroundR, "sr", "Damping", 0.05, 3, 0.8, 1, "N s/m", 2, g);
        add(kSurroundKr, "skr", "Radial stiffness", 10, 10000, 300, 1e3, "kN/m", 0, g);
        g = "Driver";
        add(kRe, "re", "Re", 2, 16, 2, 1, "Ohm", 2, g);
        add(kLe, "le", "Le", 0.05, 3, 0.5, 1e-3, "mH", 3, g);
        add(kL2, "l2", "L2 (eddy)", 0.01, 5, 1, 1e-3, "mH", 3, g);
        add(kR2, "r2", "R2 (eddy)", 0.5, 50, 7, 1, "Ohm", 2, g);
        add(kBl, "bl", "Bl", 4, 25, 4, 1, "T m", 2, g);
        add(kMotorMass, "motor", "Coil + former mass", 1, 30, 6, 1e-3, "g", 2, g);
        add(kCms, "cms", "Cms", 0.02, 0.5, 0.1, 1e-3, "mm/N", 4, g);
        add(kRms, "rms", "Rms", 0.5, 10, 3, 1, "N s/m", 2, g);
        g = "Box";
        add(kVb, "vb", "Volume", 10, 200, 50, 1e-3, "l", 0, g);
        add(kQa, "qa", "Absorption Q", 2, 100, 20, 1, "", 1, g);
        add(kOpenArea, "open", "Back opening (0 = closed)", 0, 2500, 200, 1e-4, "cm2", 0, g);
        add(kBaffle, "baffle", "Front (baffle) size", 30, 80, 45, 1e-2, "cm", 0, g);

        cab.prepare(48000);   // a scratch build for the normalisation; prepare() redoes it at the run rate
        c = &cab.circuit();
        el = nullptr;
        inVolts = inVoltsDefault = 20;
        nominalGain = measureGain();
        auto node = [&](const char* s2) { return cab.circuit().node(s2); };
        prb = { { "amp (V)", node("amp") }, { "coil back-EMF (V)", node("coil") }, { "cone velocity (m/s)", node("u") } };
    }

    void prepare(double sampleRate) override
    {
        cab.prepare(sampleRate);
        c = &cab.circuit();
    }
    void warmStart() override { cab.circuit().warmStart(); }
    const acoustic::Cab* cabModel() const override { return &cab; }
    double process(double x) override { return cab.process(x * inVolts) / (nominalGain * inVoltsDefault) * outGain; }

protected:
    void applyOne(const Control& q, double raw) override
    {
        const size_t k = (size_t) (&q - ctl.data());
        if (k < index.size() && index[k] >= 0) { cab.set(index[k], raw * q.scale); return; }
        if (!std::strcmp(q.key, "amp")) { inVolts = raw; return; }
        BenchCircuit::applyOne(q, raw);
    }
    void rebuild() override { cab.applyCircuit(); }

private:
    acoustic::Cab cab;
    std::vector<int> index;   // per control: the CabParam it sets (-1: handled here)

    // mic output per amp volt at 1 kHz with the defaults: the level that plays at unity
    double measureGain()
    {
        const double fs = 48000, f = 1000;
        double pk = 0;
        for (int i = 0; i < 4800; ++i) {
            const double y = cab.process(std::sin(2 * M_PI * f * i / fs));
            if (i > 2400) pk = std::max(pk, std::abs(y));
        }
        cab.circuit().warmStart();
        return pk;
    }
};

inline std::vector<std::string> names() { return { "Tube mic pre", "Single-ended output (Iron)", "Mic transformer", "LA-2A leveler", "Tokyo '68 fuzz (FY-2)", "Speaker cab (1x12)" }; }

inline std::unique_ptr<BenchCircuit> make(int index)
{
    switch (index) {
    case 0: return std::make_unique<TubePreBench>();
    case 1: return std::make_unique<SEOutputBench>();
    case 2: return std::make_unique<MicTransformerBench>();
    case 3: return std::make_unique<LA2ABench>();
    case 4: return std::make_unique<ShinEiBench>();
    case 5: return std::make_unique<CabBench>();
    default: return nullptr;
    }
}

} // namespace cd::catalog
