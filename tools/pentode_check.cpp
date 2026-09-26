// pentode_check -- the 6AQ5 defaults in Devices.h Pentode against the
// datasheet typical operating point (250 V plate, 250 V screen, -12.5 V grid:
// 45 mA plate, 4.5 mA screen), plus a few plate curves for sanity.
#include "circuit/Devices.h"

#include <cstdio>

int main()
{
    cd::net::Pentode t;
    double i[3], J[9];
    const double v[3] = { -12.5, 250, 250 };
    t.eval(v, i, J);
    std::printf("6AQ5 @ 250/250/-12.5 V: plate %.1f mA (datasheet 45), screen %.2f mA (datasheet 4.5)\n", i[0] * 1e3, i[1] * 1e3);
    std::printf("plate curves (screen 250 V):\n");
    for (double vg : { 0.0, -5.0, -10.0, -12.5, -15.0, -20.0 }) {
        std::printf("  vg1 %6.1f:", vg);
        for (double vp : { 25.0, 50.0, 100.0, 200.0, 300.0 }) {
            const double vv[3] = { vg, vp, 250 };
            t.eval(vv, i, J);
            std::printf("  %3.0fV %5.1fmA", vp, i[0] * 1e3);
        }
        std::printf("\n");
    }
    // derivative check against finite differences
    const double h = 1e-4;
    double ip[3], im[3], Jd[9];
    t.eval(v, i, J);
    double maxRel = 0;
    for (int c = 0; c < 3; ++c) {
        double vp[3] = { v[0], v[1], v[2] }, vm[3] = { v[0], v[1], v[2] };
        vp[c] += h; vm[c] -= h;
        t.eval(vp, ip, Jd);
        t.eval(vm, im, Jd);
        for (int r = 0; r < 3; ++r) {
            const double fd = (ip[r] - im[r]) / (2 * h);
            if (std::abs(fd) > 1e-9) maxRel = std::max(maxRel, std::abs(J[r * 3 + c] - fd) / std::abs(fd));
        }
    }
    std::printf("analytic vs finite-difference Jacobian: max relative error %.2e\n", maxRel);
    return 0;
}
