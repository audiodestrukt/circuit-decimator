// Radiation.h -- from cone velocity to the pressure a microphone picks up: the
// IR-building half of the cab simulator (docs/briefs/2026-09-26-speaker-cab.md,
// "physics -> IR"). Plain C++, no JUCE; allocation happens only in build().
//
// Rayleigh integral over the radiating surface, with the mic in the near field:
//
//   p(mic) = (j w rho / 2 pi) * sum_e  u_e dS_e  e^{-jkR}/R  * g(psi, kR)
//
// where e runs over small patches of the cone and dust cap, R is the patch's
// distance to a point on the mic's diaphragm, and g is the mic's pickup:
//
//   g = alpha + (1 - alpha) cos(psi) (1 + 1/(jkR))
//
// alpha = 1 omni (pressure), 0.5 cardioid, 0 figure-8 (pressure gradient);
// psi = angle between the mic's axis and the direction the sound arrives from.
// The 1/(jkR) term is the near-field part of the particle velocity: proximity
// effect comes out of the geometry. Capsule size = averaging over points on the
// diaphragm disc.
//
// Because j w (1/(jkR)) = c/R, the whole transfer is
//
//   H(w) = rho/(2 pi) * [ j w A(w) + c B(w) ],
//   A = sum w1_e e^{-j w tau_e},  w1 = dS/R (alpha + (1-alpha) cos psi)
//   B = sum w2_e e^{-j w tau_e},  w2 = dS/R^2 (1-alpha) cos psi,   tau = R/c
//
// A and B are built in the time domain as trains of band-limited (windowed
// sinc) fractional-delay impulses, so the cost is patches x diaphragm points x
// taps, not x frequency bins; then one FFT, the j w / c combination, and back.
//
// Geometry: the baffle is the plane z = 0, the mic in front (z > 0). The cone
// is recessed: its patches sit at their true depth (their delays and distances
// are real), radiating with their projected area (the cone's volume velocity is
// Sd u whatever its shape). Using the baffled free-field kernel for recessed
// sources is the usual approximation; the cone's own shading of the mic is
// ignored.
//
// Cone breakup (Cone.h) makes each ring's velocity frequency dependent, so the
// build is split in two cached halves:
//   rings()       each ring's radiation to the mic, H_j(w)   -- when the mic moves
//   coneTransfer  each ring's motion per unit coil motion, T_j(w) -- when the cone changes
//   combine()     H(w) = sum_j T_j(w) H_j(w), then the IR
// A rigid piston is T_j = 1.
#pragma once

#include "Bessel.h"
#include "Cone.h"
#include "MicModels.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace cd::acoustic {

inline constexpr double kRho = 1.204, kC = 343.0;


struct Mic {
    double offset = 0.0;         // across the cone from its axis (m): 0 = on the dust cap
    double distance = 0.025;     // from the baffle / grille plane (m)
    double angle = 0.0;          // tilt of the mic's axis in the offset plane (rad): 0 = square to the baffle,
                                 //   positive = pointing back towards the cone's centre
    double capsule = 0.02;       // diaphragm diameter (m)
    double pattern = 0.5;        // 1 omni, 0.5 cardioid, 0 figure-8 (the ideal mic)
    int model = 0;               // 0 ideal (pattern above, flat); 1 dynamic cardioid, measured (MicModels.h)
    double face = 0.0;           // the mic's front face (grille) diameter (m); 0 = acoustically transparent
};

// The mic isn't transparent: its front face reflects the sound arriving from the
// speaker back down to the surface below it (dust cap, cone or baffle), which
// reflects it back up to the capsule, a round trip of 2d later (d: mic face to
// that surface along the mic's axis -- the cone is recessed, so d changes as the
// mic moves across it). The face is a disc of radius a: on its axis at distance z,
// its reflection of a normally incident wave is (Kirchhoff, Fresnel zones)
//   e^{-jkz} - e^{-jk sqrt(z^2 + a^2)}
// relative to an infinite wall's (weak while the disc is small against the
// wavelength, strong above). Tilting the mic by theta sends the return off at
// 2 theta: a disc's directivity 2 J1(x)/x, x = k a sin 2 theta. One round trip;
// the surface reflects with rs (paper cone and dust cap ~0.8, baffle ~0.9).
// Returns 1 + Gamma: the factor on the pressure at the capsule.
inline std::complex<double> micReflection(const Cone& cone, const Mic& mic, double f)
{
    if (mic.face <= 0 || f <= 0) return 1.0;
    const double k = 2 * M_PI * f / kC, a = mic.face / 2;
    const double r = std::abs(mic.offset);
    const bool overCone = r < cone.radius;
    const double zs = overCone ? coneHeight(cone, r) : 0.0;
    const double d = std::max(0.002, (mic.distance - zs) / std::max(0.2, std::cos(mic.angle)));
    const double rs = overCone ? 0.8 : 0.9;
    const double z = 2 * d;
    const std::complex<double> disc = std::polar(1.0, -k * z) - std::polar(1.0, -k * std::sqrt(z * z + a * a));
    const double x = k * a * std::abs(std::sin(2 * mic.angle));
    const double tilt = x < 1e-9 ? 1.0 : 2 * besselJ(1, x) / x;
    return 1.0 + rs * tilt * disc;
}


// ---- a small radix-2 FFT (in place) ---------------------------------------------
inline void fft(std::vector<std::complex<double>>& a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2 * M_PI / (double) len * (inverse ? 1 : -1);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= (double) n;
}

// The minimum-phase response with magnitude `mag` (bins 0..n/2 of an n-point FFT),
// by the real cepstrum: a measured capsule response is causal, and applying it
// with zero phase would put part of the IR before t = 0.
inline std::vector<std::complex<double>> minimumPhase(const std::vector<double>& mag, size_t n)
{
    std::vector<std::complex<double>> c(n);
    for (size_t k = 0; k <= n / 2; ++k) {
        const double l = std::log(std::max(mag[k], 1e-9));
        c[k] = l;
        if (k > 0 && k < n / 2) c[n - k] = l;
    }
    fft(c, true);                       // real cepstrum
    for (size_t i = 1; i < n / 2; ++i) { c[i] *= 2.0; c[n - i] = 0.0; }   // fold onto positive quefrency
    fft(c, false);
    std::vector<std::complex<double>> out(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) out[k] = std::exp(c[k]);
    return out;
}

struct Patch {
    double x, y, z, area;   // centre (m), projected area (m^2)
    double ring;            // radius of its ring (for modal weights later)
};

// The radiating surface as patches no larger than `size` (m): dust cap dome
// (r < dustCap) and cone (dustCap..radius), recessed below z = 0.
// The radiating surface as rings of patches no larger than `size` (m): dust cap
// dome (r < dustCap) and cone (dustCap..radius), recessed below z = 0. The cone
// under the dust cap is hidden and doesn't radiate outwards.
struct Ring {
    double radius;
    std::vector<Patch> patches;
};
inline std::vector<Ring> surface(const Cone& c, double size)
{
    std::vector<Ring> rings;
    const int n = std::max(4, (int) std::ceil(c.radius / size));
    const double dr = c.radius / n;
    for (int i = 0; i < n; ++i) {
        const double r0 = i * dr, r1 = r0 + dr, rm = 0.5 * (r0 + r1);
        const int segs = std::max(1, (int) std::ceil(2 * M_PI * rm / size));
        const double area = M_PI * (r1 * r1 - r0 * r0) / segs;
        Ring ring { rm, {} };
        for (int s = 0; s < segs; ++s) {
            const double ph = 2 * M_PI * (s + 0.5) / segs;
            ring.patches.push_back({ rm * std::cos(ph), rm * std::sin(ph), coneHeight(c, rm), area, rm });
        }
        rings.push_back(std::move(ring));
    }
    return rings;
}

// Points on the mic's diaphragm (a disc of diameter capsule, facing along the
// mic's axis) with area weights summing to 1.
struct DiaphragmPoint {
    double x, y, z, w;
};
inline std::vector<DiaphragmPoint> diaphragm(const Mic& m, double ax, double az)
{
    const double r = 0.5 * m.capsule;
    const double cx = m.offset, cy = 0, cz = m.distance;
    // two unit vectors spanning the diaphragm plane: y, and the axis rotated 90 degrees in x-z
    const double ux = az, uz = -ax;
    std::vector<DiaphragmPoint> pts { { cx, cy, cz, 0 } };
    if (r < 1e-4) { pts[0].w = 1; return pts; }
    // 19 equal-area cells: a centre disc, then annuli of 6 and 12 cells, each
    // point at its annulus's area centroid radius
    const double b0 = std::sqrt(1.0 / 19), b1 = std::sqrt(7.0 / 19), b2 = 1.0;
    auto centroid = [](double lo, double hi) { return 2.0 / 3.0 * (hi * hi * hi - lo * lo * lo) / (hi * hi - lo * lo); };
    const double rr[] = { r * centroid(b0, b1), r * centroid(b1, b2) };
    const int nn[] = { 6, 12 };
    pts[0].w = 1.0 / 19;
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < nn[k]; ++i) {
            const double ph = 2 * M_PI * (i + 0.5 * k) / nn[k];
            const double a = rr[k] * std::cos(ph), b = rr[k] * std::sin(ph);
            pts.push_back({ cx + a * ux, cy + b, cz + a * uz, 1.0 / 19 });
        }
    return pts;
}

// ---- stage 1: each ring's radiation to the mic ------------------------------------
// H[j][k]: pressure at the mic (pascal-equivalent: the omni part is pressure, the
// gradient part is scaled to pressure by rho c) per unit velocity of ring j, bin k
// (k = 0..n/2 at fs). Bins above maxFreq are tapered off.
struct MicRings {
    double fs = 48000;
    size_t n = 0;
    std::vector<double> radius;
    std::vector<std::vector<std::complex<double>>> H;
    double delay = 0;   // shortest time of flight (s)
    // the mic's own per-bin terms, for other sources reaching it (an open back's rear wave)
    std::vector<double> alpha;                      // pressure/gradient mix
    std::vector<std::complex<double>> response;     // its on-axis response (minimum phase; 1 for the ideal mic)
};

inline MicRings rings(const Cone& cone, const Mic& mic, double fs, size_t n, double maxFreq = 20000)
{
    MicRings out;
    out.fs = fs;
    out.n = n;
    const double lambda = kC / std::min(maxFreq, 0.5 * fs);
    const auto surf = surface(cone, std::min(lambda / 6, cone.radius / 8));
    // mic axis: pointing from the mic towards the baffle (-z), tilted by angle towards -x (the centre)
    const double ax = -std::sin(mic.angle), az = -std::cos(mic.angle);
    const auto pts = diaphragm(mic, ax, az);
    const double alphaIdeal = std::clamp(mic.pattern, 0.0, 1.0);
    constexpr int half = 16;   // windowed-sinc half width (taps)
    auto deposit = [&](std::vector<double>& buf, double tSamples, double w) {
        const int i0 = (int) std::floor(tSamples);
        for (int k = i0 - half + 1; k <= i0 + half; ++k) {
            if (k < 0 || k >= (int) n) continue;
            const double x = (double) k - tSamples;
            const double sinc = std::abs(x) < 1e-12 ? 1.0 : std::sin(M_PI * x) / (M_PI * x);
            const double win = 0.5 * (1 + std::cos(M_PI * x / half));   // Hann
            buf[(size_t) k] += w * sinc * win;
        }
    };
    const double t0 = 0.42 * fs, t1 = std::min(0.5 * fs, std::max(t0 + 1, maxFreq * 1.05));
    double minTau = 1e9;
    // three trains per ring, so the pattern may change with frequency:
    //   A0 = sum dS/R (pressure), A1 = sum dS/R cos psi, B1 = sum dS/R^2 cos psi (gradient)
    //   H = rho/2pi [ j w (alpha A0 + (1 - alpha) A1) + c (1 - alpha) B1 ]
    std::vector<double> A0(n), A1(n), B1(n);
    std::vector<std::complex<double>> a0(n), a1(n), b1(n);
    // per-bin mic factors: the pattern mix, the capsule's own response, the face reflection
    std::vector<double> alpha(n / 2 + 1), respMag(n / 2 + 1, 1.0), taper(n / 2 + 1);
    std::vector<std::complex<double>> refl(n / 2 + 1);
    for (size_t k = 0; k <= n / 2; ++k) {
        const double f = fs * (double) k / (double) n;
        alpha[k] = mic.model == 1 ? mics::dynamicAlpha(std::max(f, 1.0)) : alphaIdeal;
        if (mic.model == 1) respMag[k] = mics::dynamicResponse(std::max(f, 1.0));
        refl[k] = micReflection(cone, mic, f);
        const double lo = std::min(t0, maxFreq);   // taper the top of the band (the differentiator)
        taper[k] = f <= lo ? 1.0 : 0.5 * (1 + std::cos(M_PI * std::min(1.0, (f - lo) / std::max(1.0, t1 - lo))));
    }
    const auto resp = mic.model == 1 ? minimumPhase(respMag, n) : std::vector<std::complex<double>>(n / 2 + 1, 1.0);
    out.alpha = alpha;
    out.response = resp;
    for (const auto& ring : surf) {
        std::fill(A0.begin(), A0.end(), 0.0);
        std::fill(A1.begin(), A1.end(), 0.0);
        std::fill(B1.begin(), B1.end(), 0.0);
        for (const auto& d : pts)
            for (const auto& p : ring.patches) {
                const double dx = d.x - p.x, dy = d.y - p.y, dz = d.z - p.z;
                const double R = std::sqrt(dx * dx + dy * dy + dz * dz);
                // cos psi = mic axis . (patch - mic) / R
                const double cosPsi = (-dx * ax - dz * az) / R;
                const double tau = R / kC;
                minTau = std::min(minTau, tau);
                deposit(A0, tau * fs, d.w * p.area / R);
                deposit(A1, tau * fs, d.w * p.area / R * cosPsi);
                deposit(B1, tau * fs, d.w * p.area / (R * R) * cosPsi);
            }
        for (size_t i = 0; i < n; ++i) { a0[i] = A0[i]; a1[i] = A1[i]; b1[i] = B1[i]; }
        fft(a0, false);
        fft(a1, false);
        fft(b1, false);
        std::vector<std::complex<double>> h(n / 2 + 1);
        for (size_t k = 0; k <= n / 2; ++k) {
            const double w = 2 * M_PI * fs * (double) k / (double) n;
            const double al = alpha[k];
            h[k] = kRho / (2 * M_PI) * (std::complex<double>(0, w) * (al * a0[k] + (1 - al) * a1[k]) + kC * (1 - al) * b1[k])
                   * resp[k] * refl[k] * taper[k];
        }
        out.radius.push_back(ring.radius);
        out.H.push_back(std::move(h));
    }
    out.delay = minTau;
    return out;
}

// ---- stage 2: the cone's motion per ring and bin, from the shell model ------------------
// T[j][k] for rings at `radius`; bins above maxFreq aren't solved (stage 1 tapers them).
struct ConeTransfer {
    std::vector<std::vector<std::complex<double>>> T;
    std::vector<std::complex<double>> Z;   // the cone's mechanical impedance at the neck, force / coil velocity, per bin
    double rigidMass = 0;
};

inline ConeTransfer coneTransfer(const ConeModel& model, const std::vector<double>& radius, double fs, size_t n,
                                 double maxFreq = 20000)
{
    ConeTransfer out;
    out.T.assign(radius.size(), std::vector<std::complex<double>>(n / 2 + 1, 1.0));
    out.Z.assign(n / 2 + 1, 0.0);
    out.rigidMass = model.rigidMass();
    std::vector<cplx> nodeT;
    for (size_t k = 1; k <= n / 2; ++k) {
        const double f = fs * (double) k / (double) n;
        if (f > std::min(0.5 * fs, maxFreq * 1.05)) break;
        model.solve(2 * M_PI * f, nodeT);
        for (size_t j = 0; j < radius.size(); ++j) out.T[j][k] = model.at(nodeT, radius[j]);
        out.Z[k] = model.lastNeckForce() / std::complex<double>(0, 2 * M_PI * f);
    }
    return out;
}

// ---- combine: H = sum_j T_j H_j, then the IR -------------------------------------------
struct RadiationIR {
    std::vector<double> ir;
    std::vector<std::complex<double>> spectrum;   // bins 0..n/2
    double delay = 0;
};

// `velocity` (optional): a per-bin correction of the coil velocity (Coupling.h)
inline RadiationIR combine(const MicRings& mr, const ConeTransfer* ct, const std::vector<std::complex<double>>* velocity = nullptr)
{
    RadiationIR out;
    const size_t n = mr.n;
    out.delay = mr.delay;
    out.spectrum.assign(n / 2 + 1, 0.0);
    for (size_t j = 0; j < mr.H.size(); ++j)
        for (size_t k = 0; k <= n / 2; ++k)
            out.spectrum[k] += (ct ? ct->T[j][k] : std::complex<double>(1.0)) * mr.H[j][k];
    if (velocity)
        for (size_t k = 0; k <= n / 2; ++k) out.spectrum[k] *= (*velocity)[k];
    std::vector<std::complex<double>> h(n);
    for (size_t k = 0; k <= n / 2; ++k) {
        h[k] = out.spectrum[k];
        if (k > 0 && k < n / 2) h[n - k] = std::conj(out.spectrum[k]);
    }
    h[n / 2] = std::complex<double>(h[n / 2].real(), 0.0);
    fft(h, true);
    out.ir.resize(n);
    for (size_t i = 0; i < n; ++i) out.ir[i] = h[i].real();
    return out;
}

// rigid piston: every patch moves with the voice coil
inline RadiationIR buildPiston(const Cone& cone, const Mic& mic, double fs, size_t n, double maxFreq = 20000)
{
    return combine(rings(cone, mic, fs, n, maxFreq), nullptr);
}

} // namespace cd::acoustic
