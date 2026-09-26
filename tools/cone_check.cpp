// cone_check -- the cone breakup model (core/acoustic/Cone.h) for
// sim/speaker/cone.py: T (piston-equivalent velocity / coil velocity) at a few
// radii over frequency.
//
//   cone_check [--flat] [--elements N] [--f1 Hz] [--f2 Hz] [--points N] [name value ...]
//   names: E rho h nu eta depth curve radius coil sk skr sr sm cap capr
//   --flat: depth 0 and a free outer edge (no surround, no dust cap): a driven
//           annular plate, which has an exact solution
//   --radial: (with --flat) drive the hub radially instead: in-plane vibration of the
//           annulus; prints the edge's radial displacement instead of T
//   prints: freq  then re/im of T at the neck+1, 1/4, 1/2, 3/4 of the way, the edge; then the dust cap
#include "acoustic/Cone.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace cd::acoustic;

int main(int argc, char** argv)
{
    Cone prof;
    ConeMaterial mat;
    double f1 = 20, f2 = 20000;
    int elements = 96, points = 400;
    bool flat = false, radial = false;
    for (int i = 1; i < argc; ++i) {
        const std::string k = argv[i];
        if (k == "--flat") { flat = true; continue; }
        if (k == "--radial") { radial = true; continue; }
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", k.c_str()); return 2; }
        const double v = std::atof(argv[++i]);
        if (k == "--elements") elements = (int) v;
        else if (k == "--f1") f1 = v;
        else if (k == "--f2") f2 = v;
        else if (k == "--points") points = (int) v;
        else if (k == "E") mat.youngs = v;
        else if (k == "rho") mat.density = v;
        else if (k == "h") mat.thickness = v;
        else if (k == "nu") mat.poisson = v;
        else if (k == "eta") mat.loss = v;
        else if (k == "depth") prof.depth = v;
        else if (k == "curve") prof.curve = v;
        else if (k == "radius") prof.radius = v;
        else if (k == "coil") prof.coilRadius = v;
        else if (k == "sk") mat.surroundK = v;
        else if (k == "skr") mat.surroundKr = v;
        else if (k == "sr") mat.surroundR = v;
        else if (k == "sm") mat.surroundMass = v;
        else if (k == "cap") mat.dustCapMass = v;
        else if (k == "capr") prof.dustCap = v;
        else if (k == "capkr") mat.dustCapKr = v;
        else if (k == "aniso") mat.anisotropy = v;
        else if (k == "ribs") mat.ribs = v;
        else if (k == "taper") mat.taper = v;
        else if (k == "capkrot") mat.dustCapKrot = v;
        else { std::fprintf(stderr, "unknown %s\n", k.c_str()); return 2; }
    }
    if (flat) {
        prof.depth = 0;
        mat.surroundK = mat.surroundKr = mat.surroundR = mat.surroundMass = mat.dustCapMass = mat.dustCapKr = mat.dustCapKrot = 0;
    }
    ConeModel c;
    c.build(prof, mat, elements);
    const int nn = c.numNodes();
    const int pick[5] = { 1, (nn - 1) / 4, (nn - 1) / 2, 3 * (nn - 1) / 4, nn - 1 };
    std::fprintf(stderr, "cone mass %.2f g; radii:", c.coneMass() * 1e3);
    for (int k : pick) std::fprintf(stderr, " %.4f", c.nodeRadius(k));
    std::fprintf(stderr, " m; dust cap at %.4f m\n", c.nodeRadius(c.dustCapNode()));
    std::vector<cplx> T;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < points; ++i) {
        const double f = f1 * std::pow(f2 / f1, points > 1 ? (double) i / (points - 1) : 0.0);
        if (radial) {
            c.solve(2 * M_PI * f, T, { 1.0, 0.0, 0.0 });
            const cplx u = c.lastRadial(nn - 1);
            std::printf("%.6g %.9g %.9g\n", f, u.real(), u.imag());
            continue;
        }
        c.solve(2 * M_PI * f, T);
        std::printf("%.6g", f);
        for (int k : pick) std::printf(" %.9g %.9g", T[(size_t) k].real(), T[(size_t) k].imag());
        std::printf(" %.9g %.9g\n", c.dustCapResponse().real(), c.dustCapResponse().imag());
    }
    const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * 1e3;
    std::fprintf(stderr, "%d frequencies in %.1f ms\n", points, ms);
}
