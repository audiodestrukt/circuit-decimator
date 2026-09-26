// SEOutput.h -- single-ended, plate-loaded output transformer stage as a
// netlist for the general engine. Mirrors sim/tubeout/se_out.cir (the spec):
// line source -> 12AU7 -> 4:1 gapped-steel output transformer -> 10k load.
// The plate current biases the core (initDC puts it on its initial
// magnetization curve, as after a slow power-on).
#pragma once

#include "../Devices.h"

#include <map>
#include <string>

namespace cd {

struct SEOutputParams {
    double n = 0.25, rprim = 800, llp = 10e-3, rsec = 40, lls = 0.6e-3, csec = 200e-12, rload = 10e3;
    double np = 4000, ac = 6e-4, le = 0.1, lg = 1e-4;                       // core geometry, gap
    double ms = 1.3e6, a = 100, alpha = 1e-4, k = 20, c = 0.3;              // gapped silicon steel
    double bplus = 250, rsrc = 600, rgk = 1e6, rk = 820, ck = 100e-6;
    double cgk = 1.6e-12, cgp = 1.5e-12, cpk = 0.5e-12;
    bool linearCore = false;
    double lm = 70;   // linear-core A/B reference: the core's small-signal inductance at its DC bias (H)
};

struct SEOutput {
    net::Circuit c;
    int input = -1, out = -1, plate = -1, supply = -1;
    net::JACore* core = nullptr;
    net::Triode* tube = nullptr;
    std::map<std::string, int> el;   // component name -> element index (for setValueDeferred)

    void build(const SEOutputParams& p)
    {
        using namespace net;
        auto n = [&](const char* s) { return c.node(s); };
        const int grid = n("grid"), cath = n("cath"), bp = n("bp"), pa = n("pa"), m = n("m");
        const int sp = n("sp"), sa = n("sa"), o = n("out");
        plate = n("plate");

        input = c.source(grid, GND, p.rsrc);
        el["rgk"] = c.resistor(grid, GND, p.rgk);
        el["rk"] = c.resistor(cath, GND, p.rk);
        el["ck"] = c.capacitor(cath, GND, p.ck);
        el["cgk"] = c.capacitor(grid, cath, p.cgk);
        el["cgp"] = c.capacitor(grid, plate, p.cgp);
        el["cpk"] = c.capacitor(plate, cath, p.cpk);
        Triode t;   // Koren 12AU7
        t.mu = 21.5; t.ex = 1.3; t.kg1 = 1180; t.kp = 84; t.kvb = 300;
        tube = static_cast<Triode*>(&c.device(std::make_unique<Triode>(t), Triode::ports(plate, grid, cath)));

        supply = c.source(bp, GND, 0, p.bplus, true);
        el["rprim"] = c.resistor(plate, pa, p.rprim);
        el["llp"] = c.inductor(pa, m, p.llp);
        auto jc = std::make_unique<JACore>();
        jc->np = p.np; jc->ac = p.ac; jc->le = p.le; jc->lg = p.lg;
        jc->ms = p.ms; jc->a = p.a; jc->alpha = p.alpha; jc->k = p.k; jc->c = p.c;
        jc->linear = p.linearCore; jc->lm = p.lm;
        core = static_cast<JACore*>(&c.device(std::move(jc), JACore::ports(m, bp)));
        c.idealTransformer(m, bp, sp, GND, p.n);
        el["lls"] = c.inductor(sp, sa, p.lls);
        el["rsec"] = c.resistor(sa, o, p.rsec);
        el["csec"] = c.capacitor(o, GND, p.csec);
        el["rload"] = c.resistor(o, GND, p.rload);
        out = c.output(o);
    }
};

} // namespace cd
