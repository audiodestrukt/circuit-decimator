// speaker_render -- the driver-in-a-box netlist (core/circuit/circuits/Speaker.h)
// measured the way sim/speaker/speaker.cir's AC analysis sees it: a sine at
// each frequency, settled, then a lock-in (correlation over whole cycles) for
// the cone velocity and coil current, relative to the amp voltage.
//
//   speaker_render sweep <f1> <f2> <points> [--fs Hz] [--set name=value ...]
//       prints: freq |u/V| arg(u/V) |Z| arg(Z)   (Z = V / i at the amp)
//   speaker_render step <out.dat> [--fs Hz] [--set ...]
//       a 1 V step at t = 1 ms, 0.1 s: "time velocity current"
//   speaker_render file <in.txt> <out.txt> [--fs Hz] [--set ...]
//       amp volts, one sample per line at fs -> cone velocity (m/s), one per line
//
// params: re le l2 r2 bl mms cms rms sd vb qa open panel
#include "circuit/circuits/Speaker.h"

#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace cd;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s sweep|step ...\n", argv[0]); return 2; }
    const std::string mode = argv[1];
    DriverParams d;
    BoxParams b;
    double fs = 96000;
    const int first = mode == "sweep" ? 5 : mode == "file" ? 4 : 3;
    if (argc < first) { std::fprintf(stderr, "bad arguments\n"); return 2; }
    std::map<std::string, double*> names { { "re", &d.re }, { "le", &d.le }, { "l2", &d.l2 }, { "r2", &d.r2 },
                                           { "bl", &d.bl }, { "mms", &d.mms }, { "cms", &d.cms }, { "rms", &d.rms },
                                           { "sd", &d.sd }, { "vb", &b.vb }, { "qa", &b.qa },
                                           { "open", &b.open }, { "panel", &b.panel } };
    for (int i = first; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--fs") && i + 1 < argc) fs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--set") && i + 1 < argc) {
            const std::string kv = argv[++i];
            const auto eq = kv.find('=');
            auto it = names.find(kv.substr(0, eq));
            if (it == names.end()) { std::fprintf(stderr, "unknown param %s\n", kv.c_str()); return 2; }
            *it->second = std::atof(kv.c_str() + eq + 1);
        } else { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }

    if (mode == "file") {
        SpeakerBox s;
        s.build(d, b);
        s.c.prepare(fs);
        FILE* in = std::fopen(argv[2], "r");
        FILE* o = std::fopen(argv[3], "w");
        if (!in || !o) { std::perror("file"); return 1; }
        double v;
        while (std::fscanf(in, "%lf", &v) == 1) {
            s.c.setInput(s.input, v);
            s.c.process();
            std::fprintf(o, "%.9g\n", s.c.out(s.velocity));
        }
        std::fclose(in);
        std::fclose(o);
        return 0;
    }
    if (mode == "step") {
        SpeakerBox s;
        s.build(d, b);
        s.c.prepare(fs);
        FILE* o = std::fopen(argv[2], "w");
        if (!o) { std::perror(argv[2]); return 1; }
        std::fprintf(o, "time velocity current\n");
        const long n = (long) (0.1 * fs);
        for (long i = 0; i < n; ++i) {
            const double t = i / fs;
            s.c.setInput(s.input, t >= 1e-3 ? 1.0 : 0.0);
            s.c.process();
            std::fprintf(o, "%.9g %.9g %.9g\n", t, s.c.out(s.velocity), s.c.out(s.current));
        }
        std::fclose(o);
        return 0;
    }

    const double f1 = std::atof(argv[2]), f2 = std::atof(argv[3]);
    const int pts = std::atoi(argv[4]);
    for (int k = 0; k < pts; ++k) {
        const double f = f1 * std::pow(f2 / f1, pts > 1 ? (double) k / (pts - 1) : 0.0);
        SpeakerBox s;
        s.build(d, b);
        s.c.prepare(fs);
        // settle for >= 20 time constants of the slowest part (~0.5 s), then measure whole cycles
        const long settle = (long) (std::max(0.6, 30.0 / f) * fs);
        const int cycles = std::max(4, (int) (0.1 * f));
        const long meas = (long) std::llround(cycles * fs / f);
        std::complex<double> U, I;
        const double w = 2 * M_PI * f / fs;
        for (long i = 0; i < settle + meas; ++i) {
            const double ph = w * (double) i;
            s.c.setInput(s.input, std::sin(ph));
            s.c.process();
            if (i >= settle) {
                const std::complex<double> e = std::polar(1.0, -ph);
                U += s.c.out(s.velocity) * e;
                I += s.c.out(s.current) * e;
            }
        }
        // input sin(ph) = Im e^{j ph}; its phasor is -j (times 1/2 in the correlation)
        const std::complex<double> Vin(0, -0.5 * (double) meas);
        const auto hu = U / Vin, z = Vin / I;
        std::printf("%.6g %.9g %.6g %.9g %.6g\n", f, std::abs(hu), std::arg(hu), std::abs(z), std::arg(z));
    }
}
