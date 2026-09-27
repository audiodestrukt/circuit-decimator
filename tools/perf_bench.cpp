// perf_bench -- CPU cost of the realtime workloads, for regression tracking
// (tests/perf_check.py compares a run against tests/perf_baseline.json).
//
//   perf_bench [--quick] > results.json
//
// Timing is the thread's own CPU time (not wall clock), best of several runs,
// so background load doesn't read as a regression. Every run also times a fixed
// reference workload (FFTs and complex arithmetic, like the engines' inner loops);
// each benchmark is reported both raw and normalised by it, so results compare
// across machines (a CI runner vs a workstation). Newton iterations per sample are
// counted too: they're deterministic, so they catch a solver slowing down even
// when timing noise would hide it.
#include "acoustic/CabModel.h"
#include "circuit/FuzzFaceDK.h"
#include "circuit/circuits/LA2A.h"
#include "circuit/circuits/SEOutput.h"
#include "circuit/circuits/ShinEi.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace {

double cpuSeconds()
{
    timespec t {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return (double) t.tv_sec + 1e-9 * (double) t.tv_nsec;
}

// best (minimum) CPU time of `reps` runs of fn, in seconds
double best(int reps, const std::function<void()>& fn)
{
    double b = 1e30;
    for (int r = 0; r < reps; ++r) {
        const double t0 = cpuSeconds();
        fn();
        b = std::min(b, cpuSeconds() - t0);
    }
    return b;
}

volatile double sink = 0;   // keeps results alive

// a guitar-ish test signal: a decaying pluck train plus a little noise
double signal(long i, double fs)
{
    const double t = (double) i / fs, ph = std::fmod(t, 0.25);
    return 0.4 * std::exp(-6 * ph) * std::sin(2 * M_PI * 110 * t) * (1 + 0.3 * std::sin(2 * M_PI * 330 * t))
           + 0.001 * std::sin(12345.678 * t * t);
}

struct Result {
    std::string name, unit;
    double seconds;        // per unit
    double iterations;     // Newton iterations per sample, or -1
};

} // namespace

int main(int argc, char** argv)
{
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    const int reps = quick ? 3 : 5;
    const double secondsAudio = quick ? 0.25 : 0.5;
    std::vector<Result> out;

    // ---- reference workload: 4096-point complex FFTs + complex exp, fixed size
    double ref;
    {
        cd::acoustic::Fft fft(4096);
        std::vector<std::complex<double>> a(4096);
        for (size_t i = 0; i < a.size(); ++i) a[i] = std::polar(1.0, 0.001 * (double) (i * i));
        const int loops = 200;
        ref = best(reps, [&] {
                  for (int l = 0; l < loops; ++l) {
                      fft.forward(a.data());
                      for (auto& x : a) x = std::exp(std::complex<double>(0, std::arg(x) * 0.5)) * std::min(1.0, std::abs(x) + 1e-3);
                      fft.inverse(a.data());
                  }
                  sink = sink + a[7].real();
              }) / loops;
        out.push_back({ "reference", "per FFT loop", ref, -1 });
    }

    // ---- speaker cab: the realtime path (driver circuit + partitioned convolution)
    {
        const double fs = 192000;
        cd::acoustic::Cab cab;
        cab.prepare(fs);
        const long n = (long) (secondsAudio * fs);
        for (long i = 0; i < 4096; ++i) cab.process(signal(i, fs));   // warm up
        const double t = best(reps, [&] {
            double acc = 0;
            for (long i = 0; i < n; ++i) acc += cab.process(20 * signal(i, fs));
            sink = sink + acc;
        });
        out.push_back({ "cab_realtime_192k", "per sample", t / (double) n, -1 });
        cab.release();
    }

    // ---- speaker cab: IR rebuilds (the worker's cost per knob move), at the Bench's rate
    {
        using namespace cd::acoustic;
        const double fs = 192000;
        const size_t irn = 8192;
        auto p = legend1258();
        CabIRBuilder b;
        b.build(p, fs, irn);
        struct Move { const char* name; int param; double a, b; };
        const Move moves[] = { { "cab_rebuild_mic", kMicOffset, 0.02, 0.03 },
                               { "cab_rebuild_cone", kYoungs, 4.5e9, 5.0e9 },
                               { "cab_rebuild_driver", kBl, 10.5, 11.0 } };
        for (const auto& m : moves) {
            int flip = 0;
            const double t = best(reps, [&] {
                p[(size_t) m.param] = (flip++ & 1) ? m.a : m.b;
                sink = sink + b.build(p, fs, irn)[100];
            });
            out.push_back({ m.name, "per rebuild", t, -1 });
        }
    }

    // ---- LA-2A leveler as Opto runs it: 2x oversampling (96 kHz), sidechain every 2
    {
        const double fs = 96000;
        cd::LA2A a;
        a.build(cd::LA2AParams {});
        a.prepare(fs, 2);
        const long n = (long) (secondsAudio * fs);
        long iters = 0, samples = 0;
        const double t = best(reps, [&] {
            double acc = 0;
            for (long i = 0; i < n; ++i) {
                a.process(0.8 * signal(i, fs));
                acc += a.output();
                iters += a.c.lastIterations;
                ++samples;
            }
            sink = sink + acc;
        });
        out.push_back({ "la2a_opto_96k", "per sample", t / (double) n, (double) iters / (double) samples });
    }

    // ---- Iron: the single-ended output stage at 4x oversampling (192 kHz)
    {
        const double fs = 192000;
        cd::SEOutput s;
        s.build(cd::SEOutputParams {});
        s.c.maxIterations = 12;
        s.c.prepare(fs);
        const long n = (long) (secondsAudio * fs);
        long iters = 0, samples = 0;
        const double t = best(reps, [&] {
            double acc = 0;
            for (long i = 0; i < n; ++i) {
                s.c.setInput(s.input, 2.0 * signal(i, fs));
                s.c.process();
                acc += s.c.out(s.out);
                iters += s.c.lastIterations;
                ++samples;
            }
            sink = sink + acc;
        });
        out.push_back({ "iron_se_192k", "per sample", t / (double) n, (double) iters / (double) samples });
    }

    // ---- Phys Fuzz: London '66 (hand-built DK solver) and Tokyo '68 (netlist) at 192 kHz
    {
        const double fs = 192000;
        cd::FuzzFaceDK ff;
        ff.prepare(fs);
        const long n = (long) (secondsAudio * fs);
        long iters = 0, samples = 0;
        const double t = best(reps, [&] {
            double acc = 0;
            for (long i = 0; i < n; ++i) {
                acc += ff.process(0.15 * signal(i, fs));
                iters += ff.lastIterations;
                ++samples;
            }
            sink = sink + acc;
        });
        out.push_back({ "fuzz_london_192k", "per sample", t / (double) n, (double) iters / (double) samples });
    }
    {
        const double fs = 192000;
        cd::ShinEi s;
        s.build(cd::ShinEiParams {});
        s.c.prepare(fs);
        const long n = (long) (secondsAudio * fs);
        long iters = 0, samples = 0;
        const double t = best(reps, [&] {
            double acc = 0;
            for (long i = 0; i < n; ++i) {
                s.c.setInput(s.input, 0.15 * signal(i, fs));
                s.c.process();
                acc += s.c.out(s.out);
                iters += s.c.lastIterations;
                ++samples;
            }
            sink = sink + acc;
        });
        out.push_back({ "fuzz_tokyo_192k", "per sample", t / (double) n, (double) iters / (double) samples });
    }

    std::printf("{\n  \"reference_seconds\": %.6e,\n  \"benchmarks\": {\n", ref);
    for (size_t i = 0; i < out.size(); ++i) {
        const auto& r = out[i];
        std::printf("    \"%s\": { \"unit\": \"%s\", \"seconds\": %.6e, \"normalized\": %.6e, \"iterations\": %.4f }%s\n",
                    r.name.c_str(), r.unit.c_str(), r.seconds, r.seconds / ref, r.iterations, i + 1 < out.size() ? "," : "");
    }
    std::printf("  }\n}\n");
    return 0;
}
