// la2a_render -- the LA-2A netlist (core/circuit/circuits/LA2A.h) on the same
// stimulus as sim/la2a/la2a.cir: a 1 kHz tone at lv1 dBu, stepping to lv2
// between t1 and t2. Writes "time v(out) v(el) v(j) R(cell)" per sample.
//
//   la2a_render out.dat [--lv1 dBu] [--lv2 dBu] [--t1 s] [--t2 s] [--dur s]
//                       [--fs Hz] [--gain 0..1] [--peak 0..1] [--every N] [--linear]
//   la2a_render out.dat --file in.txt [...]   input "time volts" pairs instead
#include "circuit/circuits/LA2A.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s out.dat [opts]\n", argv[0]); return 2; }
    double lv1 = -30, lv2 = 0, t1 = 0.3, t2 = 1.3, dur = 2.5, fs = 96000, freq = 1000;
    int every = 1;
    const char* file = nullptr;
    cd::LA2AParams p;
    for (int i = 2; i < argc; ++i) {
        auto arg = [&](const char* name) { return !std::strcmp(argv[i], name) && i + 1 < argc; };
        if (arg("--lv1")) lv1 = std::atof(argv[++i]);
        else if (arg("--lv2")) lv2 = std::atof(argv[++i]);
        else if (arg("--t1")) t1 = std::atof(argv[++i]);
        else if (arg("--t2")) t2 = std::atof(argv[++i]);
        else if (arg("--dur")) dur = std::atof(argv[++i]);
        else if (arg("--fs")) fs = std::atof(argv[++i]);
        else if (arg("--freq")) freq = std::atof(argv[++i]);
        else if (arg("--gain")) p.gain = std::atof(argv[++i]);
        else if (arg("--peak")) p.peak = std::atof(argv[++i]);
        else if (arg("--every")) every = std::atoi(argv[++i]);
        else if (arg("--file")) file = argv[++i];
        else if (!std::strcmp(argv[i], "--linear")) p.in.linearCore = p.out.linearCore = true;
        else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    std::vector<double> ts, vs;
    if (file) {
        FILE* f = std::fopen(file, "r");
        if (!f) { std::perror(file); return 1; }
        double t, v;
        while (std::fscanf(f, "%lf %lf", &t, &v) == 2) { ts.push_back(t); vs.push_back(v); }
        std::fclose(f);
        dur = ts.back();
    }

    cd::LA2A a;
    a.build(p);
    a.prepare(fs, every);
    std::printf("bias: k1a %.3f p1a %.2f k2a %.2f sink %.2f p4 %.2f V\n", a.c.x(a.c.node("k1a")), a.c.x(a.c.node("p1a")),
                a.c.x(a.c.node("k2a")), a.c.x(a.c.node("sink")), a.sc.x(a.sc.node("p4")));

    auto amp = [](double d) { return 0.775 * std::sqrt(2.0) * std::pow(10.0, d / 20); };
    const long n = (long) (dur * fs);
    FILE* o = std::fopen(argv[1], "w");
    if (!o) { std::perror(argv[1]); return 1; }
    std::fprintf(o, "time v(out) v(el) v(j) rcell\n");
    size_t k = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) {
        const double t = i / fs;
        double vin;
        if (file) {
            while (k + 1 < ts.size() && ts[k + 1] <= t) ++k;
            const double f = k + 1 < ts.size() ? (t - ts[k]) / (ts[k + 1] - ts[k]) : 0;
            vin = vs[k] + f * (k + 1 < ts.size() ? vs[k + 1] - vs[k] : 0);
        } else {
            vin = (t > t1 && t < t2 ? amp(lv2) : amp(lv1)) * std::sin(2 * M_PI * freq * t);
        }
        a.process(vin);
        std::fprintf(o, "%.9g %.9g %.9g %.9g %.9g\n", t, a.output(), a.sc.x(a.elNode), a.c.x(a.jNode), a.t4->resistance());
    }
    std::fclose(o);
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%.2f s of audio in %.2f s, %ld failures\n", dur, s, a.c.failures + a.sc.failures);
}
