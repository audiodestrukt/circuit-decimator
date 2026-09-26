// net_selftest -- the general engine (core/circuit/Circuit.h) must reproduce
// the hand-built solvers it generalises.
//
//   1. transformer, referred form, vs core/circuit/Transformer.h
//   2. transformer, physical form (ideal transformer element) vs form 1
//   3. fuzz circuit as a netlist vs core/circuit/FuzzFaceDK.h (riff input)
//
//   net_selftest [riff input.txt]
#include "circuit/Devices.h"
#include "circuit/FuzzFaceDK.h"
#include "circuit/Transformer.h"

#include <chrono>
#include <cstdio>
#include <vector>

using namespace cd;
using namespace cd::net;

static constexpr double FS = 192000;

// ---- transformer netlists --------------------------------------------------
static int buildTransformer(Circuit& c, const TransformerParams& p, bool physical, int& out)
{
    const int b1 = c.node("b1"), m = c.node("m"), b2 = c.node("b2"), s = c.node("s"), z = c.node("z");
    const int in = c.source(b1, GND, p.rsrc + p.rp);
    c.inductor(b1, m, p.ll1);
    auto core = std::make_unique<JACore>();
    core->np = p.np; core->ac = p.ac; core->le = p.le;
    core->ms = p.ms; core->a = p.a; core->alpha = p.alpha; core->k = p.k; core->c = p.c;
    c.device(std::move(core), JACore::ports(m, GND));
    if (physical) {
        // real secondary values behind an ideal 1:n transformer
        const int sp = c.node("sp");
        c.idealTransformer(m, GND, sp, GND, p.n);
        c.inductor(sp, b2, p.ll2);
        c.resistor(b2, s, p.rs);
        c.capacitor(s, GND, p.cs);
        c.resistor(s, GND, p.rload);
        c.resistor(s, z, p.rz);
        c.capacitor(z, GND, p.cz);
        out = c.output(s);
    } else {
        const double n2 = p.n * p.n;
        c.inductor(m, b2, p.ll2 / n2);
        c.resistor(b2, s, p.rs / n2);
        c.capacitor(s, GND, p.cs * n2);
        c.resistor(s, GND, p.rload / n2);
        c.resistor(s, z, p.rz / n2);
        c.capacitor(z, GND, p.cz * n2);
        out = c.output(s, GND, p.n);
    }
    return in;
}

// ---- fuzz netlist (sim/fuzzface.cir) ---------------------------------------
static int buildFuzz(Circuit& c, const FuzzFaceParams& p, int& out)
{
    auto n = [&](const char* s) { return c.node(s); };
    const int vp = n("vp"), p1 = n("p1"), gin = n("gin"), b1 = n("b1"), c1 = n("c1"), c2 = n("c2");
    const int e2 = n("e2"), tap = n("tap"), fzw = n("fzw"), o1 = n("o1"), o = n("out");
    auto pot = [](double frac, double r) { return std::max(frac * r, 1.0); };
    c.source(vp, GND, p.rbat, p.vcc, true);            // battery (Norton)
    const int in = c.source(p1, GND, p.rsrc);          // pickup EMF
    c.capacitor(vp, GND, p.cbulk);
    c.inductor(p1, gin, p.lpu);
    c.capacitor(gin, GND, p.ccab);
    c.capacitor(gin, b1, p.cin);
    c.resistor(gin, b1, p.rleakCin);
    c.resistor(c1, b1, p.rleak1);
    c.resistor(vp, c1, p.rc1);
    c.resistor(c2, c1, p.rleak2);
    c.resistor(c2, tap, p.rc2a);
    c.resistor(tap, vp, p.rc2b);
    c.resistor(e2, b1, p.rfb);
    c.resistor(e2, fzw, pot(1 - p.fuzz, p.rfuzz));
    c.resistor(fzw, GND, pot(p.fuzz, p.rfuzz));
    c.capacitor(fzw, GND, p.cfz);
    c.capacitor(tap, o1, p.cout);
    c.resistor(o1, o, pot(1 - p.vol, p.rvol));
    c.resistor(o, GND, pot(p.vol, p.rvol));
    c.resistor(o, GND, p.rload);
    const double bfs[2] = { p.bf1, p.bf2 };
    const int cs[2] = { c1, c2 }, bs[2] = { b1, c1 }, es[2] = { GND, e2 };
    for (int q = 0; q < 2; ++q)
        c.device(std::make_unique<Bjt>(bjt::model(p.isat, bfs[q], p.vaf, p.tempC)), Bjt::ports(cs[q], bs[q], es[q]));
    out = c.output(o);
    return in;
}

static void report(const char* name, const std::vector<double>& a, const std::vector<double>& b, double wall, double dur)
{
    double err = 0, ref = 0, mx = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        err += (a[i] - b[i]) * (a[i] - b[i]);
        ref += b[i] * b[i];
        mx = std::max(mx, std::abs(a[i] - b[i]));
    }
    std::printf("%-44s rms err/rms %.2e  max |err| %.2e V  engine %.1fx realtime\n", name,
                std::sqrt(err / (ref + 1e-30)), mx, dur / wall);
}

int main(int argc, char** argv)
{
    // 1 + 2: transformer, 1 V pk 50 Hz with the deck's 4-cycle fade-in
    {
        TransformerParams tp;
        Transformer ref;
        ref.setParams(tp);
        ref.prepare(FS);
        Circuit ca, cb;
        int oa, ob;
        const int ia = buildTransformer(ca, tp, false, oa);
        const int ib = buildTransformer(cb, tp, true, ob);
        ca.reltol = cb.reltol = 1e-7;
        ca.prepare(FS);
        cb.prepare(FS);
        const double f = 50, dur = 12 / f;
        const long n = (long) (dur * FS);
        std::vector<double> yr(n), ya(n), yb(n);
        const auto t0 = std::chrono::steady_clock::now();
        for (long i = 0; i < n; ++i) {
            const double t = i / FS, vin = std::sin(2 * M_PI * f * t) * std::min(1.0, t * f / 4);
            yr[i] = ref.process(vin);
            ca.setInput(ia, vin);
            ca.process();
            ya[i] = ca.out(oa);
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        for (long i = 0; i < n; ++i) {
            const double t = i / FS, vin = std::sin(2 * M_PI * f * t) * std::min(1.0, t * f / 4);
            cb.setInput(ib, vin);
            cb.process();
            yb[i] = cb.out(ob);
        }
        report("1. transformer netlist vs Transformer.h", ya, yr, wall, dur);
        report("2. physical (ideal xfmr) vs referred form", yb, ya, wall, dur);
    }

    // 3: fuzz netlist vs FuzzFaceDK on the riff (or a test tone)
    {
        std::vector<double> ts, vs;
        if (argc > 1) {
            FILE* f = std::fopen(argv[1], "r");
            double t, v;
            while (f && std::fscanf(f, "%lf %lf", &t, &v) == 2) { ts.push_back(t); vs.push_back(v); }
            if (f) std::fclose(f);
        }
        if (ts.size() < 2) {
            for (int i = 0; i <= 48000; ++i) { ts.push_back(i / 48000.0); vs.push_back(0.1 * std::sin(2 * M_PI * 110 * i / 48000.0)); }
        }
        FuzzFaceParams fp;
        FuzzFaceDK ref;
        ref.setParams(fp);
        ref.prepare(FS);
        Circuit c;
        int o;
        const int in = buildFuzz(c, fp, o);
        c.maxIterations = 8;
        c.prepare(FS);
        std::printf("   fuzz bias: engine v(c2) %.4f V, FuzzFaceDK %.4f V\n", c.x(c.node("c2")), ref.node(FuzzFace::C2));
        const double dur = ts.back();
        const long n = (long) (dur * FS);
        std::vector<double> yr(n), ye(n);
        size_t k = 0;
        double wall = 0;
        for (long i = 0; i < n; ++i) {
            const double t = i / FS;
            while (k + 1 < ts.size() && ts[k + 1] < t) ++k;
            const double a = std::clamp((t - ts[k]) / (ts[k + 1] - ts[k]), 0.0, 1.0);
            const double vin = vs[k] + a * (vs[k + 1] - vs[k]);
            yr[i] = ref.process(vin);
            const auto t0 = std::chrono::steady_clock::now();
            c.setInput(in, vin);
            c.process();
            wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            ye[i] = c.out(o);
        }
        report("3. fuzz netlist vs FuzzFaceDK", ye, yr, wall, dur);
    }
    return 0;
}
