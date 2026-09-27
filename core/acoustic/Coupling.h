// Coupling.h -- the cone's load on the motor (the brief's "cut at the voice-coil
// force"). The per-sample circuit (Speaker.h) drives a fixed moving mass Mms,
// datasheet style, with the air load included. In reality, above breakup the
// outer cone decouples and the air load fades above ka ~ 1, so the coil pushes
// less mass and moves faster. Linear, so it goes into the IR as a per-bin
// correction of the circuit's coil velocity:
//
//   C(w) = u_true / u_circuit = (Ze Zm + Bl^2) / (Ze Zm_true + Bl^2)
//
//   Zm      = j w Mms + 1/(j w Cms) + Rms + box                   (the circuit)
//   Zm_true = j w (Mms - M_air - M_cone) + Zcone(w) + 2 Zrad(w)
//             + 1/(j w Cms) - k_s/(j w) + (Rms - R_s) + box
//
// Zcone is the shell model's force per coil velocity at the neck, with the
// surround's stiffness k_s and damping R_s, which are part of Cms and Rms, so
// they're taken out of the lumped terms. M_cone is the shell model's rigid
// mass, M_air the low-frequency air load of both sides (2 x 8 rho a^3 / 3),
// Zrad the radiation impedance of a baffled piston (each side):
//   Zrad = rho c S [1 - 2 J1(2ka)/(2ka) + j 2 H1(2ka)/(2ka)]
// At low frequency Zcone -> j w M_cone + k_s/(j w) + R_s and Zrad -> j w M_air/2,
// so C -> 1: the verified circuit is untouched where it's valid.
#pragma once

#include "Radiation.h"
#include "../circuit/circuits/Speaker.h"

#include <cmath>
#include <complex>
#include <vector>

namespace cd::acoustic {

// Struve H1: the power series up to x = 8 (exact to rounding), then the Aarts &
// Janssen (2003) approximation (absolute error < 0.005, small against H1 there)
inline double struveH1(double x)
{
    if (x < 1e-12) return 0.0;
    if (x < 8) {
        // H1 = sum_k (-1)^k (x/2)^(2k+2) / (Gamma(k + 3/2) Gamma(k + 5/2))
        double term = (x / 2) * (x / 2) / (std::tgamma(1.5) * std::tgamma(2.5)), sum = term;
        for (int k = 1; k < 60; ++k) {
            term *= -(x / 2) * (x / 2) / ((k + 0.5) * (k + 1.5));
            sum += term;
            if (std::abs(term) < 1e-17 * std::abs(sum)) break;
        }
        return sum;
    }
    return 2 / M_PI - std::cyl_bessel_j(0.0, x) + (16 / M_PI - 5) * std::sin(x) / x
           + (12 - 36 / M_PI) * (1 - std::cos(x)) / (x * x);
}

// mechanical radiation impedance of one side of a baffled piston of radius a
inline std::complex<double> pistonRadiation(double w, double a)
{
    const double k = w / kC, x = 2 * k * a, S = M_PI * a * a;
    if (x < 1e-6) return { 0.0, 0.0 };
    return kRho * kC * S * std::complex<double>(1 - 2 * std::cyl_bessel_j(1.0, x) / x, 2 * struveH1(x) / x);
}

struct CoupledDriver {
    // per bin: the electrical impedance of the coil (with the amp's R), the circuit's
    // mechanical impedance, and the true one (cone and air load as they really are)
    std::vector<std::complex<double>> ze, zm, zt;
    // per bin: the share of the cone's rear air flow that leaves through the back
    // opening (the rest compresses the box air); 0 for a closed back
    std::vector<std::complex<double>> rearFraction;
    double bl2 = 0, rAmp = 0;
};

inline CoupledDriver coupledDriver(const DriverParams& d, const BoxParams& b, double rAmp, const ConeTransfer& ct,
                                   const ConeMaterial& mat, double radius, double fs, size_t n)
{
    CoupledDriver out;
    out.ze.assign(n / 2 + 1, 0.0);
    out.zm.assign(n / 2 + 1, 0.0);
    out.zt.assign(n / 2 + 1, 0.0);
    out.rearFraction.assign(n / 2 + 1, 0.0);
    // the back opening: the circuit's air plug (constant radiation resistance) and the
    // true one (inner end correction + a baffled piston's radiation impedance outside)
    const double mp = openingMass(b), rpc = openingResistance(b);
    const bool open = b.open > 0 && mp < kClosedMass;
    const double ro = open ? std::sqrt(b.open / M_PI) : 0.0;
    out.bl2 = d.bl * d.bl;
    out.rAmp = rAmp;
    const double mAir = 2 * 8 * kRho * radius * radius * radius / 3;
    const double cab = b.vb / (kRho * kC * kC);
    const double wc = 2 * M_PI * closedBoxFc(d, b);
    const double rab = std::isfinite(b.qa) && b.qa > 0 ? 1 / (wc * cab * b.qa) : 0.0;
    std::complex<double> zcLast = 0.0;
    double wLast = 1;
    for (size_t k = 1; k <= n / 2; ++k) {
        const double w = 2 * M_PI * fs * (double) k / (double) n;
        const std::complex<double> s(0, w);
        out.ze[k] = rAmp + d.re + s * d.le + (s * d.l2 * d.r2) / (s * d.l2 + d.r2);
        const auto zair = 1.0 / (s * cab) + rab;                            // box air + absorption
        const std::complex<double> zpc = s * mp + rpc;                      // opening, as the circuit has it
        const std::complex<double> zpt = open
            ? s * (kRho * (b.panel + 0.85 * ro) / b.open) + pistonRadiation(w, ro) / (b.open * b.open)
            : zpc;
        const auto box = d.sd * d.sd * zair * zpc / (zair + zpc);
        const auto boxTrue = d.sd * d.sd * zair * zpt / (zair + zpt);
        out.rearFraction[k] = open ? zair / (zair + zpt) : 0.0;
        out.zm[k] = s * d.mms + 1.0 / (s * d.cms) + d.rms + box;
        // above the solved band: hold the cone's last impedance (as a mass-like term scaled with w)
        std::complex<double> zc = ct.Z[k];
        if (!(std::abs(zc) > 0)) zc = zcLast * (w / wLast);
        else { zcLast = zc; wLast = w; }
        out.zt[k] = s * (d.mms - mAir - ct.rigidMass) + zc + 2.0 * pistonRadiation(w, radius)
                    + 1.0 / (s * d.cms) - mat.surroundK / s + (d.rms - mat.surroundR) + boxTrue;
    }
    return out;
}

// C(w) = u_true / u_circuit per bin
inline std::vector<std::complex<double>> velocityCorrection(const CoupledDriver& cd)
{
    std::vector<std::complex<double>> C(cd.ze.size(), 1.0);
    for (size_t k = 1; k < C.size(); ++k)
        C[k] = (cd.ze[k] * cd.zm[k] + cd.bl2) / (cd.ze[k] * cd.zt[k] + cd.bl2);
    return C;
}

// the driver's input impedance with the true mechanical load (amp resistance excluded)
inline std::vector<std::complex<double>> inputImpedance(const CoupledDriver& cd)
{
    std::vector<std::complex<double>> Z(cd.ze.size(), 0.0);
    for (size_t k = 1; k < Z.size(); ++k) Z[k] = cd.ze[k] - cd.rAmp + cd.bl2 / cd.zt[k];
    return Z;
}

inline std::vector<std::complex<double>> velocityCorrection(const DriverParams& d, const BoxParams& b, double rAmp,
                                                            const ConeTransfer& ct, const ConeMaterial& mat,
                                                            double radius, double fs, size_t n)
{
    return velocityCorrection(coupledDriver(d, b, rAmp, ct, mat, radius, fs, n));
}

} // namespace cd::acoustic
