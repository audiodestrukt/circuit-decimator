// MicTransformer.h -- the 1:10 mic input transformer on its own, as a netlist
// (physical form: primary, Jiles-Atherton core, ideal 1:n, real secondary).
// Mirrors sim/transformer/xfmr_core.cir (the spec; that deck uses the
// referred form, which tools/net_selftest shows is identical).
#pragma once

#include "../Devices.h"
#include "../Transformer.h"

#include <map>
#include <string>

namespace cd {

struct MicTransformer {
    net::Circuit c;
    int input = -1, out = -1;
    net::JACore* core = nullptr;
    std::map<std::string, int> el;

    void build(const TransformerParams& p)
    {
        using namespace net;
        auto n = [&](const char* s) { return c.node(s); };
        const int b1 = n("b1"), m = n("m"), sp = n("sp"), b2 = n("b2"), s = n("s"), z = n("z");
        input = c.source(b1, GND, p.rsrc + p.rp);
        el["ll1"] = c.inductor(b1, m, p.ll1);
        auto jc = std::make_unique<JACore>();
        jc->np = p.np; jc->ac = p.ac; jc->le = p.le;
        jc->ms = p.ms; jc->a = p.a; jc->alpha = p.alpha; jc->k = p.k; jc->c = p.c;
        core = static_cast<JACore*>(&c.device(std::move(jc), JACore::ports(m, GND)));
        c.idealTransformer(m, GND, sp, GND, p.n);
        el["ll2"] = c.inductor(sp, b2, p.ll2);
        el["rs"] = c.resistor(b2, s, p.rs);
        el["cs"] = c.capacitor(s, GND, p.cs);
        el["rload"] = c.resistor(s, GND, p.rload);
        el["rz"] = c.resistor(s, z, p.rz);
        el["cz"] = c.capacitor(z, GND, p.cz);
        out = c.output(s);
    }
};

} // namespace cd
