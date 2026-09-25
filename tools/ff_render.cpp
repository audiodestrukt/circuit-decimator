// ff_render -- run core/circuit/FuzzFace.h offline on an ngspice-style input file.
//
//   ff_render <input.txt> <out.dat> [--fs 192000] [--iters 8] [--solver mna|dk|dkf] [name=value ...]
//
// input.txt: "time value" lines (the same file ngspice's filesource reads).
// out.dat:   "time v(out) v(vp) v(c2)" at every step, like ngspice wrdata.
// Parameter names match the .param names in sim/fuzzface.cir. --solver picks
// the reference MNA solver, the DK solver (default: what the plugins run) or
// the DK solver in single precision (the microcontroller path).
#include "circuit/FuzzFaceDK.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

template <typename Solver>
static int render(Solver ff, const cd::FuzzFaceParams& p, double fs, int iters, double reltol,
                  const std::vector<double>& ts, const std::vector<double>& vs, const char* outPath)
{
    ff.maxIterations = iters;
    if constexpr (requires { ff.reltol; }) ff.reltol = (decltype(ff.reltol)) reltol;
    else (void) reltol;
    ff.setParams(p);
    ff.prepare(fs);
    std::printf("op: v(vp)=%g v(b1)=%g v(c1)=%g v(e2)=%g v(c2)=%g\n",
                ff.node(cd::FuzzFace::VP), ff.node(cd::FuzzFace::B1), ff.node(cd::FuzzFace::C1),
                ff.node(cd::FuzzFace::E2), ff.node(cd::FuzzFace::C2));

    // same linear interpolation ngspice's filesource does between points
    const double tEnd = ts.back();
    const long n = (long) (tEnd * fs) + 1;
    std::vector<float> out((size_t) n * 4);
    size_t k = 0;
    long iterSum = 0;
    int iterMax = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < n; ++i) {
        const double t = i / fs;
        while (k + 1 < ts.size() && ts[k + 1] < t) ++k;
        const double a = std::clamp((t - ts[k]) / (ts[k + 1] - ts[k]), 0.0, 1.0);
        const double vin = vs[k] + a * (vs[k + 1] - vs[k]);
        const double y = (double) ff.process((decltype(ff.process(0))) vin);
        iterSum += ff.lastIterations;
        iterMax = std::max(iterMax, ff.lastIterations);
        out[i * 4 + 0] = (float) t;
        out[i * 4 + 1] = (float) y;
        out[i * 4 + 2] = (float) ff.node(cd::FuzzFace::VP);
        out[i * 4 + 3] = (float) ff.node(cd::FuzzFace::C2);
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    FILE* f = std::fopen(outPath, "w");
    if (!f) { std::perror(outPath); return 1; }
    std::fprintf(f, "time v(out) v(vp) v(c2)\n");
    for (long i = 0; i < n; ++i)
        std::fprintf(f, "%.9g %.9g %.9g %.9g\n", out[i * 4], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
    std::fclose(f);

    std::printf("%ld samples @ %g Hz: %.3f s wall, %.1fx realtime, newton avg %.2f max %d, failures %ld\n",
                n, fs, wall, tEnd / wall, (double) iterSum / n, iterMax, ff.failures);
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s input.txt out.dat [--fs HZ] [--iters N] [--solver mna|dk|dkf] [k=v ...]\n", argv[0]);
        return 2;
    }
    double fs = 192000;
    int iters = 8;
    double reltol = 1e-3;
    std::string solver = "dk";
    cd::FuzzFaceParams p;
    std::map<std::string, double*> names {
        { "vcc", &p.vcc }, { "rbat", &p.rbat }, { "cbulk", &p.cbulk },
        { "rsrc", &p.rsrc }, { "lpu", &p.lpu }, { "ccab", &p.ccab },
        { "cin", &p.cin }, { "rleak_cin", &p.rleakCin },
        { "rc1", &p.rc1 }, { "rc2a", &p.rc2a }, { "rc2b", &p.rc2b }, { "rfb", &p.rfb },
        { "fuzz", &p.fuzz }, { "rfuzz", &p.rfuzz }, { "cfz", &p.cfz },
        { "cout", &p.cout }, { "vol", &p.vol }, { "rvol", &p.rvol }, { "rload", &p.rload },
        { "bf1", &p.bf1 }, { "bf2", &p.bf2 }, { "isat", &p.isat }, { "vaf", &p.vaf },
        { "rleak1", &p.rleak1 }, { "rleak2", &p.rleak2 }, { "temp", &p.tempC },
    };
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) { fs = std::atof(argv[++i]); continue; }
        if (!std::strcmp(argv[i], "--iters") && i + 1 < argc) { iters = std::atoi(argv[++i]); continue; }
        if (!std::strcmp(argv[i], "--solver") && i + 1 < argc) { solver = argv[++i]; continue; }
        if (!std::strcmp(argv[i], "--reltol") && i + 1 < argc) { reltol = std::atof(argv[++i]); continue; }
        const char* eq = std::strchr(argv[i], '=');
        if (!eq) { std::fprintf(stderr, "bad arg %s\n", argv[i]); return 2; }
        auto it = names.find(std::string(argv[i], (size_t) (eq - argv[i])));
        if (it == names.end()) { std::fprintf(stderr, "unknown param %s\n", argv[i]); return 2; }
        *it->second = std::atof(eq + 1);
    }

    std::vector<double> ts, vs;
    {
        FILE* f = std::fopen(argv[1], "r");
        if (!f) { std::perror(argv[1]); return 1; }
        double t, val;
        while (std::fscanf(f, "%lf %lf", &t, &val) == 2) { ts.push_back(t); vs.push_back(val); }
        std::fclose(f);
    }
    if (ts.size() < 2) { std::fprintf(stderr, "empty input\n"); return 1; }

    if (solver == "mna") return render(cd::FuzzFace {}, p, fs, iters, 0, ts, vs, argv[2]);
    if (solver == "dkf") return render(cd::FuzzFaceDKf {}, p, fs, iters, reltol, ts, vs, argv[2]);
    return render(cd::FuzzFaceDK {}, p, fs, iters, reltol, ts, vs, argv[2]);
}
