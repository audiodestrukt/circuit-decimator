// tubepre_render -- run the tube mic pre (core/circuit/circuits/TubePre.h) offline.
//
//   tubepre_render sine <amp V pk> <freq Hz> <cycles> <out.dat> [--fs 192000] [--linear]
//   tubepre_render file <input.txt> <out.dat> [--fs 192000] [--gain G] [--linear]
//
// out.dat: "time v(out) B H v(plate)" per sample, like sim/tubepre/tubepre.cir.
// --linear swaps the transformer's hysteretic core for an ideal 8 H inductor.
#include "circuit/circuits/TubePre.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char** argv)
{
    const bool sine = argc > 1 && !std::strcmp(argv[1], "sine");
    if (argc < (sine ? 6 : 4)) {
        std::fprintf(stderr, "usage: %s sine AMP FREQ CYCLES out.dat [--fs HZ] [--linear] | file in.txt out.dat [--fs HZ] [--gain G] [--linear]\n", argv[0]);
        return 2;
    }
    double fs = 192000, gain = 1;
    bool linear = false;
    for (int i = sine ? 6 : 4; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) fs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) gain = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--linear")) linear = true;
    }
    std::vector<double> ts, vs;
    double dur, amp = 0, freq = 0;
    const char* outPath;
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

    cd::TubePreParams p;
    p.xfmr.linearCore = linear;
    cd::TubePre pre;
    pre.build(p);
    pre.c.prepare(fs);
    std::printf("bias: plate %.2f V, cathode %.3f V\n", pre.c.x(pre.plate), pre.c.x(pre.c.node("cath")));

    const long n = (long) (dur * fs);
    std::vector<float> out((size_t) n * 5);
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
        pre.c.setInput(pre.input, vin);
        pre.c.process();
        iters += pre.c.lastIterations;
        float* r = &out[(size_t) i * 5];
        r[0] = (float) t;
        r[1] = (float) pre.c.out(pre.out);
        r[2] = (float) pre.core->fluxDensity();
        r[3] = (float) pre.core->field();
        r[4] = (float) pre.c.x(pre.plate);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    FILE* f = std::fopen(outPath, "w");
    if (!f) { std::perror(outPath); return 1; }
    std::fprintf(f, "time v(out) B H v(plate)\n");
    for (long i = 0; i < n; ++i) {
        const float* r = &out[(size_t) i * 5];
        std::fprintf(f, "%.9g %.9g %.9g %.9g %.9g\n", r[0], r[1], r[2], r[3], r[4]);
    }
    std::fclose(f);
    std::printf("%ld samples @ %g Hz: %.1fx realtime, newton avg %.2f, failures %ld\n", n, fs, dur / wall,
                (double) iters / n, pre.c.failures);
    return 0;
}
