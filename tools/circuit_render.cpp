// circuit_render -- run a netlist circuit (core/circuit/circuits/) offline.
//
//   circuit_render <circuit> sine <amp V pk> <freq Hz> <cycles> <out.dat> [opts]
//   circuit_render <circuit> file <input.txt> <out.dat> [opts]
//   opts: --fs HZ  --gain G  --linear (ideal core)  --set name=value (circuit param)
//
// circuits: seout  (single-ended output transformer stage)
//           shinei (Shin-Ei FY-2 fuzz; --set takes the deck's .param names)
// out.dat: seout  "time v(out) B H v(plate)" per sample
//          shinei "time v(out) v(c1) v(c2) v(vp)" per sample
#include "circuit/circuits/SEOutput.h"
#include "circuit/circuits/ShinEi.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

int main(int argc, char** argv)
{
    if (argc < 3) { std::fprintf(stderr, "usage: %s <circuit> sine|file ...\n", argv[0]); return 2; }
    const std::string which = argv[1];
    const bool sine = !std::strcmp(argv[2], "sine");
    if (argc < (sine ? 7 : 5)) { std::fprintf(stderr, "bad arguments\n"); return 2; }
    double fs = 192000, gain = 1;
    bool linear = false;
    std::map<std::string, double> sets;
    for (int i = sine ? 7 : 5; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) fs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--gain") && i + 1 < argc) gain = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--linear")) linear = true;
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            sets[kv.substr(0, eq)] = std::atof(kv.c_str() + eq + 1);
        }
    }

    std::vector<double> ts, vs;
    double dur, amp = 0, freq = 0;
    const char* outPath;
    if (sine) {
        amp = std::atof(argv[3]);
        freq = std::atof(argv[4]);
        dur = std::atof(argv[5]) / freq;
        outPath = argv[6];
    } else {
        FILE* f = std::fopen(argv[3], "r");
        if (!f) { std::perror(argv[3]); return 1; }
        double t, v;
        while (std::fscanf(f, "%lf %lf", &t, &v) == 2) { ts.push_back(t); vs.push_back(v * gain); }
        std::fclose(f);
        dur = ts.back();
        outPath = argv[4];
    }

    // the circuit behind a common interface for the loop below
    struct Runner {
        std::function<double(double)> process;
        std::function<void(float*)> extra;   // 3 telemetry columns
        std::function<int()> iterations;
        std::function<long()> failures;
        const char* header;
    } run;
    cd::SEOutput seout;
    cd::ShinEi shinei;
    if (which == "seout") {
        cd::SEOutputParams p;
        p.linearCore = linear;
        std::map<std::string, double*> names { { "lg", &p.lg }, { "bplus", &p.bplus }, { "rk", &p.rk },
                                               { "n", &p.n }, { "np", &p.np }, { "ac", &p.ac }, { "le", &p.le },
                                               { "ms", &p.ms }, { "a", &p.a }, { "k", &p.k }, { "c", &p.c }, { "lm", &p.lm } };
        for (auto& [k, v] : sets) {
            auto it = names.find(k);
            if (it == names.end()) { std::fprintf(stderr, "unknown param %s\n", k.c_str()); return 2; }
            *it->second = v;
        }
        seout.build(p);
        seout.c.prepare(fs);
        std::printf("bias: plate %.2f V, B_dc %+.4f T, H_dc %+.2f A/m\n", seout.c.x(seout.plate),
                    seout.core->fluxDensity(), seout.core->field());
        run = { [&](double vin) { seout.c.setInput(seout.input, vin); seout.c.process(); return seout.c.out(seout.out); },
                [&](float* r) { r[0] = (float) seout.core->fluxDensity(); r[1] = (float) seout.core->field(); r[2] = (float) seout.c.x(seout.plate); },
                [&] { return seout.c.lastIterations; }, [&] { return seout.c.failures; }, "time v(out) B H v(plate)" };
    } else if (which == "shinei") {
        cd::ShinEiParams p;
        double temp = 27;
        std::map<std::string, double*> names {
            { "vcc", &p.vcc }, { "rbat", &p.rbat }, { "cbulk", &p.cbulk }, { "rsrc", &p.rsrc }, { "lpu", &p.lpu },
            { "ccab", &p.ccab }, { "cin", &p.cin }, { "rleak_cin", &p.rleakCin }, { "rb1", &p.rb1 }, { "cfb1", &p.cfb1 },
            { "rc1", &p.rc1 }, { "cc", &p.cc }, { "rb2", &p.rb2 }, { "rc2", &p.rc2 }, { "rsup", &p.rsup }, { "csup", &p.csup },
            { "fuzz", &p.fuzz }, { "rfuzz", &p.rfuzz }, { "cq1", &p.cq1 }, { "cq2", &p.cq2 }, { "ctone", &p.ctone },
            { "rtone1", &p.rtone1 }, { "rtone2", &p.rtone2 }, { "cmid", &p.cmid }, { "vol", &p.vol }, { "rvol", &p.rvol },
            { "rload", &p.rload }, { "bf1", &p.bf1 }, { "bf2", &p.bf2 }, { "isat", &p.isat }, { "vaf", &p.vaf },
            { "rleak1", &p.rleak1 }, { "rleak2", &p.rleak2 }, { "temp", &temp } };
        for (auto& [k, v] : sets) {
            auto it = names.find(k);
            if (it == names.end()) { std::fprintf(stderr, "unknown param %s\n", k.c_str()); return 2; }
            *it->second = v;
        }
        p.tempC = temp;
        shinei.build(p);
        shinei.c.maxIterations = 8;
        shinei.c.prepare(fs);
        using S = cd::ShinEi;
        std::printf("bias: v(vp)=%.4f v(b1)=%.4f v(c1)=%.4f v(b2)=%.4f v(va2)=%.4f v(c2)=%.4f\n", shinei.node(S::VP),
                    shinei.node(S::B1), shinei.node(S::C1), shinei.node(S::B2), shinei.node(S::VA2), shinei.node(S::C2));
        run = { [&](double vin) { return shinei.process(vin); },
                [&](float* r) { r[0] = (float) shinei.node(S::C1); r[1] = (float) shinei.node(S::C2); r[2] = (float) shinei.node(S::VP); },
                [&] { return shinei.c.lastIterations; }, [&] { return shinei.c.failures; }, "time v(out) v(c1) v(c2) v(vp)" };
    } else {
        std::fprintf(stderr, "unknown circuit %s\n", which.c_str());
        return 2;
    }

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
        float* r = &out[(size_t) i * 5];
        r[0] = (float) t;
        r[1] = (float) run.process(vin);
        iters += run.iterations();
        run.extra(r + 2);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    FILE* f = std::fopen(outPath, "w");
    if (!f) { std::perror(outPath); return 1; }
    std::fprintf(f, "%s\n", run.header);
    for (long i = 0; i < n; ++i) {
        const float* r = &out[(size_t) i * 5];
        std::fprintf(f, "%.9g %.9g %.9g %.9g %.9g\n", r[0], r[1], r[2], r[3], r[4]);
    }
    std::fclose(f);
    std::printf("%ld samples @ %g Hz: %.1fx realtime, newton avg %.2f, failures %ld\n", n, fs, dur / wall,
                (double) iters / n, run.failures());
    return 0;
}
