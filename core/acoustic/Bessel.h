// Bessel.h -- J0 and J1 of the first kind, for the piston and disc formulas
// (Radiation.h, Coupling.h). std::cyl_bessel_j isn't available everywhere
// (Apple's libc++ doesn't ship the C++17 special functions).
//   |x| < 12: the power series
//   beyond:   Hankel's asymptotic expansion, 5 terms
// Within 3e-9 of scipy's j0/j1 for 0 < x < 200.
#pragma once

#include <cmath>

namespace cd::acoustic {

inline double besselJ(int order, double x)   // order 0 or 1
{
    const double ax = std::abs(x);
    if (ax < 12) {
        // sum_k (-1)^k (x/2)^(2k+n) / (k! (k+n)!)
        const double h = 0.5 * x, h2 = h * h;
        double term = order == 0 ? 1.0 : h, sum = term;
        for (int k = 1; k < 80; ++k) {
            term *= -h2 / ((double) k * (double) (k + order));
            sum += term;
            if (std::abs(term) < 1e-17 * std::abs(sum) + 1e-300) break;
        }
        return sum;
    }
    // J_n(x) ~ sqrt(2/(pi x)) [P cos(chi) - Q sin(chi)], chi = x - (2n+1) pi/4
    const double mu = 4.0 * order * order, z = 8 * ax;
    double P = 1, Q = (mu - 1) / z, tp = 1, tq = (mu - 1) / z;
    for (int k = 1; k < 6; ++k) {
        const double a = 2 * k, b = 2 * k + 1;
        tp *= -(mu - (2 * a - 1) * (2 * a - 1)) * (mu - (2 * a - 3) * (2 * a - 3)) / ((a - 1) * a * z * z);
        tq *= -(mu - (2 * b - 1) * (2 * b - 1)) * (mu - (2 * b - 3) * (2 * b - 3)) / ((b - 1) * b * z * z);
        P += tp;
        Q += tq;
    }
    const double chi = ax - (2 * order + 1) * M_PI / 4;
    const double j = std::sqrt(2 / (M_PI * ax)) * (P * std::cos(chi) - Q * std::sin(chi));
    return order == 1 && x < 0 ? -j : j;
}

} // namespace cd::acoustic
