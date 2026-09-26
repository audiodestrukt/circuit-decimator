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
#include "MicTransformer.h"
#include "SEOutput.h"
#include "TubePre.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace cd::catalog {

enum class Target { Element, Ratio, Supply, Core, Tube, InputLevel, Output };

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
};

inline double dbuPeakVolts(double dbu) { return 0.775 * std::sqrt(2.0) * std::pow(10.0, dbu / 20.0); }

class BenchCircuit {
public:
    virtual ~BenchCircuit() = default;
    const std::vector<Control>& controls() const { return ctl; }
    const std::vector<Probe>& probes() const { return prb; }
    net::JACore* core() const { return coreDev; }
    net::Circuit& circuit() { return *c; }

    void prepare(double sampleRate) { c->prepare(sampleRate); }

    void apply(const double* v)
    {
        for (size_t k = 0; k < ctl.size(); ++k) {
            const auto& q = ctl[k];
            const double x = v[k] * q.scale;
            switch (q.target) {
            case Target::Element: c->setValueDeferred((*el)[q.key], x); break;
            case Target::Ratio: c->setRatioDeferred(0, x); break;
            case Target::Supply: c->setInput(supply, x); break;
            case Target::Core: setField(coreFields(), q.key, x); break;
            case Target::Tube: setField(tubeFields(), q.key, x); break;
            case Target::InputLevel: inVolts = dbuPeakVolts(v[k]); break;
            case Target::Output: outGain = std::pow(10.0, v[k] / 20.0); break;
            }
        }
        c->rebuildIfDirty();
    }

    // full-scale in -> full-scale out, normalised by the circuit's nominal gain
    // so the default settings play at roughly unity
    double process(double x)
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

inline std::vector<std::string> names() { return { "Tube mic pre", "Single-ended output (Iron)", "Mic transformer" }; }

inline std::unique_ptr<BenchCircuit> make(int index)
{
    switch (index) {
    case 0: return std::make_unique<TubePreBench>();
    case 1: return std::make_unique<SEOutputBench>();
    case 2: return std::make_unique<MicTransformerBench>();
    default: return nullptr;
    }
}

} // namespace cd::catalog
