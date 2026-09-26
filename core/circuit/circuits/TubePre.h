// TubePre.h -- tube mic preamp as a netlist for the general engine.
// Mirrors sim/tubepre/tubepre.cir (the spec): 150 ohm mic -> 1:10 input
// transformer with a Jiles-Atherton core -> 12AX7 common-cathode stage ->
// output coupling cap. Ideal 250 V supply.
#pragma once

#include "../Devices.h"
#include "../Transformer.h"

#include <map>
#include <string>

namespace cd {

struct TubePreParams {
    TransformerParams xfmr;   // input transformer (mic source is xfmr.rsrc)
    double bplus = 250, ra = 100e3, rk = 1.5e3, ck = 22e-6, rg = 2.2e3, cc = 100e-9, rout = 1e6;
    double cgk = 1.6e-12, cgp = 1.7e-12, cpk = 0.46e-12;
    net::Triode tube;          // Koren 12AX7 parameters
};

struct TubePre {
    net::Circuit c;
    int input = -1, out = -1;
    net::JACore* core = nullptr;
    net::Triode* tube = nullptr;
    int plate = -1, supply = -1;
    std::map<std::string, int> el;   // component name -> element index (for setValueDeferred)

    void build(const TubePreParams& p)
    {
        using namespace net;
        const auto& x = p.xfmr;
        auto n = [&](const char* s) { return c.node(s); };
        const int b1 = n("b1"), m = n("m"), sp = n("sp"), b2 = n("b2"), s = n("s"), z = n("z");
        const int grid = n("grid"), cath = n("cath"), bp = n("bp"), o = n("out");
        plate = n("plate");

        input = c.source(b1, GND, x.rsrc + x.rp);
        el["ll1"] = c.inductor(b1, m, x.ll1);
        auto jc = std::make_unique<JACore>();
        jc->np = x.np; jc->ac = x.ac; jc->le = x.le;
        jc->ms = x.ms; jc->a = x.a; jc->alpha = x.alpha; jc->k = x.k; jc->c = x.c;
        jc->linear = x.linearCore; jc->lm = x.lm;
        core = static_cast<JACore*>(&c.device(std::move(jc), JACore::ports(m, GND)));
        c.idealTransformer(m, GND, sp, GND, x.n);
        el["ll2"] = c.inductor(sp, b2, x.ll2);
        el["rs"] = c.resistor(b2, s, x.rs);
        el["cs"] = c.capacitor(s, GND, x.cs);
        el["rload"] = c.resistor(s, GND, x.rload);
        el["rz"] = c.resistor(s, z, x.rz);
        el["cz"] = c.capacitor(z, GND, x.cz);

        supply = c.source(bp, GND, 0, p.bplus, true);   // ideal supply rail
        el["ra"] = c.resistor(bp, plate, p.ra);
        el["rg"] = c.resistor(s, grid, p.rg);
        el["rk"] = c.resistor(cath, GND, p.rk);
        el["ck"] = c.capacitor(cath, GND, p.ck);
        el["cgk"] = c.capacitor(grid, cath, p.cgk);
        el["cgp"] = c.capacitor(grid, plate, p.cgp);
        el["cpk"] = c.capacitor(plate, cath, p.cpk);
        tube = static_cast<Triode*>(&c.device(std::make_unique<Triode>(p.tube), Triode::ports(plate, grid, cath)));
        el["cc"] = c.capacitor(plate, o, p.cc);
        el["rout"] = c.resistor(o, GND, p.rout);
        out = c.output(o);
    }
};

} // namespace cd
