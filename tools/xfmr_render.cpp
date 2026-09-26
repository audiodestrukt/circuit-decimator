// xfmr_render -- run core/circuit/Transformer.h offline.
//
//   xfmr_render sine <amp V pk> <freq Hz> <cycles> <out.dat> [--fs 192000]
//   xfmr_render file <input.txt> <out.dat> [--fs 192000] [--gain G] [--linear]
//
// --linear swaps the hysteretic core for an ideal 8 H inductor (A/B reference).
//
// sine: the same 4-cycle fade-in as sim/transformer/xfmr_core.cir.
// file: "time value" lines (ngspice filesource format), linearly interpolated.
// out.dat: "time v(out) B H" per sample, like the deck's wrdata.
#include "circuit/Transformer.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    const bool sineMode = argc > 1 && !std::strcmp(argv[1], "sine");
    if (argc < (sineMode ? 6 : 4)) {
        std::fprintf(stderr, "usage: %s sine AMP FREQ CYCLES out.dat [--fs HZ] | file in.txt out.dat [--fs HZ] [--gain G]\n", argv[0]);
        return 2;
    }
    const bool sine = !std::strcmp(argv[1], "sine");
    double fs = 192000, gain = 1;
    bool linear = false;
    for (int i = sine ? 6 : 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) fs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) gain = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--linear")) linear = true;
    }
    std::vector<double> ts, vs;
    double dur;
    const char* outPath;
    double amp = 0, freq = 0;
    if (sine) {
        amp = std::atof(argv[2]);
        freq = std::atof(argv[3]);
        dur = std::atof(argv[4]) / freq;
        outPath = argv[5];
    } else {
        FILE* f = std::fopen(argv[2], "r");
        if (!f) { std::perror(argv[2]); return 1; }
        double t, v;
        while (std::fscanf(f, "%lf %lf", &t, &v) == 2) { ts.push_back(t); vs.push_back(v * gain); }
        std::fclose(f);
        dur = ts.back();
        outPath = argv[3];
    }

    cd::Transformer xf;
    cd::TransformerParams tp;
    tp.linearCore = linear;
    xf.setParams(tp);
    xf.prepare(fs);
    const long n = (long) (dur * fs);
    std::vector<float> out((size_t) n * 4);
    size_t k = 0;
    long iters = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) {
        const double t = i / fs;
        double vin;
        if (sine) {
            vin = amp * std::sin(2 * M_PI * freq * t) * std::min(1.0, t * freq / 4);
        } else {
            while (k + 1 < ts.size() && ts[k + 1] < t) ++k;
            const double a = std::clamp((t - ts[k]) / (ts[k + 1] - ts[k]), 0.0, 1.0);
            vin = vs[k] + a * (vs[k + 1] - vs[k]);
        }
        const double y = xf.process(vin);
        iters += xf.lastIterations;
        out[(size_t) i * 4] = (float) t;
        out[(size_t) i * 4 + 1] = (float) y;
        out[(size_t) i * 4 + 2] = (float) xf.fluxDensity();
        out[(size_t) i * 4 + 3] = (float) xf.field();
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    FILE* f = std::fopen(outPath, "w");
    if (!f) { std::perror(outPath); return 1; }
    std::fprintf(f, "time v(out) B H\n");
    for (long i = 0; i < n; ++i)
        std::fprintf(f, "%.9g %.9g %.9g %.9g\n", out[(size_t) i * 4], out[(size_t) i * 4 + 1],
                     out[(size_t) i * 4 + 2], out[(size_t) i * 4 + 3]);
    std::fclose(f);
    std::printf("%ld samples @ %g Hz: %.1fx realtime, newton avg %.2f, failures %ld\n", n, fs,
                dur / wall, (double) iters / n, xf.failures);
    return 0;
}
