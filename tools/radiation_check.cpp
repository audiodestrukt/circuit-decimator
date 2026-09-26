// radiation_check -- the cone-to-mic transfer (core/acoustic/Radiation.h) for
// sim/speaker/radiation.py to compare with analytic piston results.
//
//   radiation_check spectrum|ir [--fs Hz] [--n N] [--fmax Hz]
//       [--radius m] [--depth m] [--dustcap m] [--caph m]
//       [--offset m] [--distance m] [--angle deg] [--capsule m] [--pattern 0..1]
//       [--coil m] [--sm kg] [--capkr N/m] [--capkrot] [--skr N/m] [--nu]
//       [--couple 0|1] (the cone's load on the motor; needs --breakup 1) with the driver:
//       [--re --le --l2 --r2 --bl --mms --cms --rms --sd --vb --qa] (vb huge = infinite baffle)
//       [--breakup 0|1] [--E Pa] [--rhoc kg/m3] [--h m] [--eta] [--sk N/m] [--sr Ns/m] [--capm kg] [--curve]
//   spectrum: "freq re im" per bin; ir: one sample per line; impedance: "freq re im" of the
//   driver's input impedance with the cone's true load (needs --breakup 1). Stderr: timing.
#include "acoustic/Coupling.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace cd::acoustic;

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: %s spectrum|ir [opts]\n", argv[0]); return 2; }
    const std::string mode = argv[1];
    Cone c;
    Mic m;
    double fs = 48000, fmax = 20000;
    size_t n = 4096;
    bool breakup = false, couple = false;
    ConeMaterial mat;
    cd::DriverParams drv;
    cd::BoxParams box;
    for (int i = 2; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const double v = std::atof(argv[i + 1]);
        if (k == "--fs") fs = v;
        else if (k == "--n") n = (size_t) v;
        else if (k == "--fmax") fmax = v;
        else if (k == "--radius") c.radius = v;
        else if (k == "--depth") c.depth = v;
        else if (k == "--dustcap") c.dustCap = v;
        else if (k == "--caph") c.capHeight = v;
        else if (k == "--offset") m.offset = v;
        else if (k == "--distance") m.distance = v;
        else if (k == "--angle") m.angle = v * M_PI / 180;
        else if (k == "--capsule") m.capsule = v;
        else if (k == "--pattern") m.pattern = v;
        else if (k == "--breakup") breakup = v != 0;
        else if (k == "--couple") couple = v != 0;
        else if (k == "--re") drv.re = v;
        else if (k == "--le") drv.le = v;
        else if (k == "--l2") drv.l2 = v;
        else if (k == "--r2") drv.r2 = v;
        else if (k == "--bl") drv.bl = v;
        else if (k == "--mms") drv.mms = v;
        else if (k == "--cms") drv.cms = v;
        else if (k == "--rms") drv.rms = v;
        else if (k == "--sd") drv.sd = v;
        else if (k == "--vb") box.vb = v;
        else if (k == "--qa") box.qa = v;
        else if (k == "--E") mat.youngs = v;
        else if (k == "--rhoc") mat.density = v;
        else if (k == "--h") mat.thickness = v;
        else if (k == "--eta") mat.loss = v;
        else if (k == "--sk") mat.surroundK = v;
        else if (k == "--sr") mat.surroundR = v;
        else if (k == "--capm") mat.dustCapMass = v;
        else if (k == "--capkr") mat.dustCapKr = v;
        else if (k == "--capkrot") mat.dustCapKrot = v;
        else if (k == "--skr") mat.surroundKr = v;
        else if (k == "--nu") mat.poisson = v;
        else if (k == "--curve") c.curve = v;
        else if (k == "--coil") c.coilRadius = v;
        else if (k == "--sm") mat.surroundMass = v;
        else if (k == "--aniso") mat.anisotropy = v;
        else if (k == "--ribs") mat.ribs = v;
        else if (k == "--taper") mat.taper = v;
        else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 2; }
    }
    const auto t0 = std::chrono::steady_clock::now();
    const auto mr = rings(c, m, fs, n, fmax);
    const auto t1 = std::chrono::steady_clock::now();
    RadiationIR r;
    double coneMs = 0;
    if (breakup) {
        ConeModel model;
        model.build(c, mat);
        const auto ct = coneTransfer(model, mr.radius, fs, n, fmax);
        coneMs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count() * 1e3;
        if (mode == "impedance") {
            const auto z = inputImpedance(coupledDriver(drv, box, 0.05, ct, mat, c.radius, fs, n));
            for (size_t k = 1; k < z.size(); ++k)
                std::printf("%.6g %.9g %.9g\n", fs * (double) k / (double) n, z[k].real(), z[k].imag());
            return 0;
        }
        if (couple) {
            const auto vc = velocityCorrection(drv, box, 0.05, ct, mat, c.radius, fs, n);
            r = combine(mr, &ct, &vc);
        } else {
            r = combine(mr, &ct);
        }
    } else {
        r = combine(mr, nullptr);
    }
    const double ms = std::chrono::duration<double>(t1 - t0).count() * 1e3;
    std::fprintf(stderr, "mic rings (%zu) %.1f ms, cone %.1f ms, first arrival %.3f ms\n", mr.radius.size(), ms, coneMs, r.delay * 1e3);
    if (mode == "ir")
        for (double x : r.ir) std::printf("%.9g\n", x);
    else
        for (size_t k = 0; k < r.spectrum.size(); ++k)
            std::printf("%.6g %.9g %.9g\n", fs * (double) k / (double) n, r.spectrum[k].real(), r.spectrum[k].imag());
}
