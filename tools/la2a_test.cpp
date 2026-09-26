// la2a_test -- the LA-2A netlist (core/circuit/circuits/LA2A.h): DC operating
// point, gain with the sidechain off, static compression curve, attack /
// release, and realtime cost.
//
//   la2a_test [--linear] [--peak 0..1] [--gain 0..1] [--rfb ohms]
#include "circuit/circuits/LA2A.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

using namespace cd;

static constexpr double FS = 192000;   // 4x 48k

static double dbu(double vpk) { return 20 * std::log10(vpk / std::sqrt(2.0) / 0.775); }
static double fromDbu(double d) { return 0.775 * std::sqrt(2.0) * std::pow(10.0, d / 20); }

struct Rig {
    LA2A a;
    LA2AParams p;
    explicit Rig(const LA2AParams& pp, int every = 1) : p(pp) { a.build(p); a.prepare(FS, every); }
    // steady 1 kHz at level (dBu at the source), returns output level (dBu) over the last 50 ms
    double tone(double levelDbu, double seconds, double freq = 1000)
    {
        const double amp = fromDbu(levelDbu);
        const long n = (long) (seconds * FS), tail = (long) (0.05 * FS);
        double pk = 0;
        for (long i = 0; i < n; ++i) {
            a.process(amp * std::sin(2 * M_PI * freq * i / FS));
            if (i >= n - tail) pk = std::max(pk, std::abs(a.output()));
        }
        return dbu(pk);
    }
};

int main(int argc, char** argv)
{
    LA2AParams p;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--linear")) p.in.linearCore = p.out.linearCore = true;
        else if (!std::strcmp(argv[i], "--peak") && i + 1 < argc) p.peak = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) p.gain = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--rfb") && i + 1 < argc) p.rfb = std::atof(argv[++i]);
    }

    {
        Rig r(p);
        auto& c = r.a.c;
        std::printf("DC operating point (B+ %.0f V):\n", p.bplus);
        const char* nodes[] = { "k1a", "p1a", "k1b", "p1b", "g2a", "k2a", "p2a", "sink", "k2b", "j" };
        for (auto* s : nodes) std::printf("  %-5s %8.2f V\n", s, c.x(c.node(s)));
        auto& sc = r.a.sc;
        const char* snodes[] = { "tail", "p3b", "k4", "g2", "p4" };
        for (auto* s : snodes) std::printf("  %-5s %8.2f V   (sidechain)\n", s, sc.x(sc.node(s)));
        std::printf("  I(V2) %.2f mA  I(V4) %.2f mA\n", c.x(c.node("k2b")) / p.r20 * 1e3, sc.x(sc.node("k4")) / p.r36 * 1e3);
    }

    // gain with the sidechain off (Peak Reduction at 0)
    {
        auto q = p;
        q.peak = 0;
        Rig r(q);
        const double out = r.tone(-30, 0.3);
        std::printf("\ngain, sidechain off, Gain %.2f: %.1f dB (-30 dBu in -> %.1f dBu out)\n", q.gain, out + 30, out);
        Rig r2(q);
        std::printf("  at 30 Hz %.1f dB, 15 kHz %.1f dB\n", r2.tone(-30, 0.4, 30) + 30, r2.tone(-30, 0.1, 15000) + 30);
    }

    // static curve
    std::printf("\nstatic curve, Peak Reduction %.2f (1 kHz, 1.5 s each):\n  in dBu   out dBu   GR dB   cell ohm\n", p.peak);
    double unity = 0;
    {
        auto q = p;
        q.peak = 0;
        Rig r(q);
        unity = r.tone(-30, 0.3) + 30;
    }
    for (double lv = -40; lv <= 10.1; lv += 5) {
        Rig r(p);
        const double o = r.tone(lv, 1.5);
        std::printf("  %6.1f   %7.1f   %5.1f   %9.0f\n", lv, o, lv + unity - o, r.a.t4->resistance());
    }

    // cost
    for (int every : { 1, 4 }) {
        Rig r(p, every);
        const long n = (long) FS;
        const double amp = fromDbu(0);
        const auto t0 = std::chrono::steady_clock::now();
        long iters = 0;
        for (long i = 0; i < n; ++i) {
            r.a.process(amp * std::sin(2 * M_PI * 1000 * i / FS));
            iters += r.a.c.lastIterations;
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("\ncost, sidechain every %d: 1 s at 192 kHz in %.2f s (%.1fx realtime, mono), %.2f Newton it/sample, %ld failures, ports %d + %d\n",
                    every, s, 1 / s, (double) iters / n, r.a.c.failures + r.a.sc.failures, r.a.c.numPorts(), r.a.sc.numPorts());
    }
}
