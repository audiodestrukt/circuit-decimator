// t4_test -- the T4 cell (core/circuit/Devices.h T4Cell) in the LA-2A's
// attenuator (UA manual fig. 5: R6 68k -> R7 2.7k -> photocell to ground,
// loaded by the 100k Gain pot), measured against UA's published behaviour:
//   attack "very fast"; ~40-80 ms to 50% release; 0.5-5 s to complete release,
//   longer after more / longer gain reduction.
// A small 1 kHz pilot tone measures the gain; the EL panel gets tone bursts.
//
//   t4_test [--curve out.csv]     prints metrics; --curve writes GR(t) for plotting
#include "circuit/Devices.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace cd::net;

static constexpr double FS = 96000;

struct Run {
    std::vector<double> t, gr;   // gain reduction in dB, one value per pilot cycle
};

// burst: EL drive amplitude (V peak, 1 kHz) for `on` seconds, then `after` seconds dark
static Run run(double elAmp, double on, double after, const T4Cell& proto)
{
    Circuit c;
    const int src = c.node("src"), j = c.node("j"), att = c.node("att"), el = c.node("el");
    const int pilot = c.source(src, GND, 600);            // 600 ohm source
    c.resistor(src, j, 68e3);                               // R6
    c.resistor(j, att, 2.7e3);                              // R7
    c.resistor(att, GND, 100e3);                            // Gain pot load
    const int drive = c.source(el, GND, 0);                 // EL panel drive (ideal source)
    c.resistor(el, GND, 1e6);
    auto& t4 = static_cast<T4Cell&>(c.device(std::make_unique<T4Cell>(proto), T4Cell::ports(el, GND, att, GND)));
    (void) t4;
    const int out = c.output(att);
    c.prepare(FS);
    Run r;
    const double dark = 100e3 * 1.0 / (100e3 + 70.7e3 + 600);   // divider with the cell dark (~5 MOhm)
    const int per = (int) (FS / 1000);
    const long n = (long) ((on + after) * FS);
    double peak = 0;
    for (long i = 0; i < n; ++i) {
        const double t = i / FS, s = std::sin(2 * M_PI * 1000 * t);
        c.setInput(pilot, 0.01 * s);
        c.setInput(drive, t < on ? elAmp * s : 0.0);
        c.process();
        peak = std::max(peak, std::abs(c.out(out)));
        if ((i + 1) % per == 0) {
            r.t.push_back(t);
            r.gr.push_back(-20 * std::log10(peak / (0.01 * dark)));
            peak = 0;
        }
    }
    return r;
}

struct Metrics { double gr, attack90, half, full; };

static Metrics measure(const Run& r, double on)
{
    Metrics m {};
    // steady GR: mean over the last 20 ms of the burst
    double s = 0; int k = 0;
    for (size_t i = 0; i < r.t.size(); ++i) if (r.t[i] > on - 0.02 && r.t[i] <= on) { s += r.gr[i]; ++k; }
    m.gr = s / std::max(k, 1);
    m.attack90 = m.half = m.full = -1;
    for (size_t i = 0; i < r.t.size(); ++i) if (r.gr[i] >= 0.9 * m.gr) { m.attack90 = r.t[i]; break; }
    for (size_t i = 0; i < r.t.size(); ++i) {
        if (r.t[i] <= on) continue;
        if (m.half < 0 && r.gr[i] <= 0.5 * m.gr) m.half = r.t[i] - on;
        if (m.full < 0 && r.gr[i] <= 1.0) { m.full = r.t[i] - on; break; }
    }
    return m;
}

int main(int argc, char** argv)
{
    const char* curve = nullptr;
    for (int i = 1; i < argc; ++i) if (!std::strcmp(argv[i], "--curve") && i + 1 < argc) curve = argv[++i];
    T4Cell proto;

    std::printf("static curve (EL drive V peak -> gain reduction):\n  ");
    for (double a : { 20.0, 40.0, 80.0, 120.0, 160.0, 240.0 }) {
        const auto r = run(a, 1.5, 0.0, proto);
        std::printf("%4.0f V: %5.1f dB   ", a, measure(r, 1.5).gr);
    }
    std::printf("\n\n%-34s %8s %10s %12s %12s\n", "burst", "GR dB", "attack90", "50% release", "to <1 dB");
    struct Case { const char* name; double amp, on; };
    const Case cases[] = { { "light, short (0.3 s)", 60, 0.3 }, { "light, long (4 s)", 60, 4.0 },
                           { "heavy, short (0.3 s)", 200, 0.3 }, { "heavy, long (4 s)", 200, 4.0 } };
    FILE* f = curve ? std::fopen(curve, "w") : nullptr;
    if (f) std::fprintf(f, "case,t,gr\n");
    for (auto& cs : cases) {
        const auto r = run(cs.amp, cs.on, 8.0, proto);
        const auto m = measure(r, cs.on);
        std::printf("%-34s %8.1f %8.1f ms %9.0f ms %10.2f s\n", cs.name, m.gr, m.attack90 * 1e3, m.half * 1e3, m.full);
        if (f) for (size_t i = 0; i < r.t.size(); ++i) std::fprintf(f, "\"%s\",%.4f,%.3f\n", cs.name, r.t[i] - cs.on, r.gr[i]);
    }
    if (f) std::fclose(f);
    return 0;
}
