// MicModels.h -- measured microphone data for the cab's mic (Radiation.h).
//
// "Dynamic cardioid (SM57 datasheet)": from the manufacturer's user guide
// (sim/speaker/reference/mic/), curves extracted from the PDF's vector paths:
//   - pattern: per-frequency pressure/gradient mix alpha(f), fitted to the six
//     published polar curves (125 Hz .. 8 kHz) together with a 16 mm receiving
//     aperture (the capsule's diaphragm averaging); 0.72 dB RMS over 0..120 deg
//   - on-axis response (far field), dB re 1 kHz
//   - grille (front face) diameter 32 mm, from the dimension drawing
// Between the tabulated points: linear in log frequency; outside, the end values.
#pragma once

#include <algorithm>
#include <cmath>

namespace cd::acoustic::mics {

struct Point { double f, v; };

inline double interp(const Point* t, int n, double f)
{
    if (f <= t[0].f) return t[0].v;
    if (f >= t[n - 1].f) return t[n - 1].v;
    for (int i = 1; i < n; ++i)
        if (f <= t[i].f) {
            const double u = std::log(f / t[i - 1].f) / std::log(t[i].f / t[i - 1].f);
            return t[i - 1].v + u * (t[i].v - t[i - 1].v);
        }
    return t[n - 1].v;
}

inline constexpr Point kDynamicAlpha[] = { { 125, 0.53 }, { 500, 0.48 }, { 1000, 0.63 }, { 2000, 0.51 }, { 4000, 0.45 }, { 8000, 0.45 } };
inline constexpr double kDynamicAperture = 0.016;   // m, diameter
inline constexpr double kDynamicFace = 0.032;       // m, grille diameter

inline constexpr Point kDynamicResponse[] = {
    { 40.0, -8.02 },
    { 42.4, -8.02 },
    { 44.9, -8.02 },
    { 47.5, -8.02 },
    { 50.4, -7.94 },
    { 53.4, -7.38 },
    { 56.5, -6.81 },
    { 59.9, -6.25 },
    { 63.4, -5.69 },
    { 67.2, -5.13 },
    { 71.2, -4.56 },
    { 75.4, -4.00 },
    { 79.9, -3.44 },
    { 84.6, -2.88 },
    { 89.6, -2.32 },
    { 94.9, -1.75 },
    { 100.5, -1.20 },
    { 106.5, -0.71 },
    { 112.8, -0.33 },
    { 119.5, -0.05 },
    { 126.6, 0.17 },
    { 134.1, 0.30 },
    { 142.1, 0.34 },
    { 150.5, 0.36 },
    { 159.4, 0.33 },
    { 168.9, 0.31 },
    { 178.9, 0.28 },
    { 189.5, 0.26 },
    { 200.7, 0.24 },
    { 212.6, 0.18 },
    { 225.2, 0.13 },
    { 238.6, 0.07 },
    { 252.8, 0.01 },
    { 267.7, -0.05 },
    { 283.6, -0.10 },
    { 300.4, -0.19 },
    { 318.3, -0.26 },
    { 337.1, -0.32 },
    { 357.1, -0.37 },
    { 378.3, -0.41 },
    { 400.7, -0.45 },
    { 424.5, -0.47 },
    { 449.7, -0.49 },
    { 476.3, -0.50 },
    { 504.6, -0.50 },
    { 534.5, -0.49 },
    { 566.2, -0.47 },
    { 599.8, -0.44 },
    { 635.3, -0.41 },
    { 673.0, -0.42 },
    { 712.9, -0.37 },
    { 755.2, -0.37 },
    { 800.0, -0.33 },
    { 847.4, -0.28 },
    { 897.7, -0.23 },
    { 950.9, -0.16 },
    { 1007.3, -0.09 },
    { 1067.1, -0.01 },
    { 1130.3, 0.08 },
    { 1197.4, 0.18 },
    { 1268.4, 0.29 },
    { 1343.6, 0.42 },
    { 1423.3, 0.49 },
    { 1507.7, 0.57 },
    { 1597.1, 0.68 },
    { 1691.8, 0.80 },
    { 1792.1, 0.94 },
    { 1898.4, 1.10 },
    { 2011.0, 1.27 },
    { 2130.2, 1.48 },
    { 2256.6, 1.69 },
    { 2390.4, 1.94 },
    { 2532.1, 2.20 },
    { 2682.3, 2.48 },
    { 2841.4, 2.79 },
    { 3009.9, 3.11 },
    { 3188.3, 3.47 },
    { 3377.4, 3.90 },
    { 3577.7, 4.18 },
    { 3789.9, 4.20 },
    { 4014.6, 4.35 },
    { 4252.7, 4.56 },
    { 4504.9, 4.74 },
    { 4772.0, 4.73 },
    { 5055.0, 4.72 },
    { 5354.8, 4.69 },
    { 5672.4, 4.54 },
    { 6008.7, 4.48 },
    { 6365.1, 4.24 },
    { 6742.5, 3.35 },
    { 7142.4, 1.88 },
    { 7565.9, 1.24 },
    { 8014.6, 2.11 },
    { 8489.9, 3.19 },
    { 8993.4, 3.55 },
    { 9526.7, 4.00 },
    { 10091.6, 3.87 },
    { 10690.1, 2.97 },
    { 11324.0, 1.66 },
    { 11995.6, 0.08 },
    { 12706.9, -1.69 },
    { 13460.5, -3.47 },
    { 14258.7, -5.24 },
    { 15104.3, -6.72 },
    { 16000.0, -6.72 }
};

inline double dynamicAlpha(double f) { return interp(kDynamicAlpha, (int) std::size(kDynamicAlpha), f); }
inline double dynamicResponse(double f)   // linear gain
{
    return std::pow(10.0, interp(kDynamicResponse, (int) std::size(kDynamicResponse), f) / 20);
}

} // namespace cd::acoustic::mics
