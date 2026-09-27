// cab_check -- the realtime cab (core/acoustic/CabModel.h) against the offline
// chain: a sine through Cab::process() (circuit + partitioned convolution with the
// worker-built IR) vs the frequency-domain product u(w) H(w) the IR came from.
// Also times the realtime path and an IR rebuild after a mic move.
//
//   cab_check [--fs Hz]
#include "acoustic/CabModel.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace cd::acoustic;

int main(int argc, char** argv)
{
    double fs = 192000;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--fs")) fs = std::atof(argv[++i]);

    // reference: circuit velocity/V (exact network) x the IR's own spectrum
    const auto p = legend1258();
    CabIRBuilder b;
    const size_t n = 8192;
    const auto ir = b.build(p, fs, n);
    std::vector<std::complex<double>> H(ir.begin(), ir.end());
    fft(H, false);

    Cab cab;
    cab.prepare(fs);
    std::printf("fs %.0f: IR %zu samples (%.1f ms), latency %d samples\n", fs, n, 1e3 * n / fs, cab.latency());
    double worst = 0;
    for (double f : { 100.0, 300.0, 1000.0, 2300.0, 4000.0, 8000.0 }) {
        cd::SpeakerBox s;   // exact velocity/V from a fresh circuit, lock-in measured
        Cab c2;
        c2.prepare(fs);
        const long settle = (long) (0.5 * fs), meas = (long) std::llround(std::max(4.0, 0.05 * f) * fs / f);
        std::complex<double> Y, U;
        const double w = 2 * M_PI * f / fs;
        for (long i = 0; i < settle + meas; ++i) {
            const double x = std::sin(w * (double) i);
            const double y = c2.process(x);
            if (i >= settle) {
                Y += y * std::polar(1.0, -w * (double) (i - c2.latency()));
                U += c2.coilVelocity() * std::polar(1.0, -w * (double) i);
            }
        }
        // expected: (coil velocity phasor) x H at f
        const double bin = f * (double) n / fs;
        const size_t k0 = (size_t) bin;
        const double fr = bin - (double) k0;
        (void) fr;
        // evaluate the IR's DTFT exactly at f
        std::complex<double> Hf = 0;
        for (size_t m = 0; m < n; ++m) Hf += ir[m] * std::polar(1.0, -w * (double) m);
        const auto expect = U * Hf;
        const double err = 20 * std::log10(std::abs(Y) / std::abs(expect));
        const double ph = std::arg(Y / expect) * 180 / M_PI;
        worst = std::max(worst, std::abs(err));
        std::printf("  %6.0f Hz: realtime vs u(w) H(w): %+.4f dB, %+.3f deg\n", f, err, ph);
        c2.release();
    }
    std::printf("worst %.4f dB\n", worst);
    const bool accurate = worst < 1.0;   // the realtime path must stay within 1 dB of the chain it's built from

    // cost of the realtime path
    const long N = (long) fs;
    const auto t0 = std::chrono::steady_clock::now();
    double acc = 0;
    for (long i = 0; i < N; ++i) acc += cab.process(std::sin(0.01 * (double) i));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("realtime path: 1 s at %.0f Hz in %.3f s (%.0fx realtime) [%g]\n", fs, s, 1 / s, acc * 0);

    // rebuild after a mic move, then a cone change, then a driver change (worker thread)
    auto wait = [&](int before) {
        const auto t = std::chrono::steady_clock::now();
        while (cab.rebuilds.load() == before && std::chrono::steady_clock::now() - t < std::chrono::seconds(10)) {
            for (int i = 0; i < 64; ++i) cab.process(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return cab.lastBuildMs.load();
    };
    int r0 = cab.rebuilds.load();
    cab.set(kMicOffset, 0.06);
    std::printf("rebuild after a mic move:     %.0f ms\n", wait(r0));
    r0 = cab.rebuilds.load();
    cab.set(kYoungs, 3e9);
    std::printf("rebuild after a cone change:  %.0f ms\n", wait(r0));
    r0 = cab.rebuilds.load();
    cab.set(kBl, 12);
    cab.applyCircuit();
    std::printf("rebuild after a driver change: %.0f ms\n", wait(r0));
    cab.release();
    if (!accurate) std::printf("FAILED: realtime cab differs from its reference by %.2f dB (limit 1 dB)\n", worst);
    return accurate ? 0 : 1;
}
