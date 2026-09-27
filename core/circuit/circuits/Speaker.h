// Speaker.h -- a loudspeaker driver in a closed box as a netlist for the
// general engine (the lumped, per-sample half of the cab simulator; see
// docs/briefs/2026-09-26-speaker-cab.md). Mirrors sim/speaker/speaker.cir.
//
// Three domains, one circuit. The electrical side is ordinary: amp (a voltage
// source with a small output resistance) -> voice coil Re, Le, and the lossy
// "semi-inductance" L2 || R2. The mechanical and acoustic sides use the
// MOBILITY analogy (velocity <-> voltage, force <-> current), which turns
// both couplings into ideal transformers:
//
//   motor:  v_mech = v_coil / Bl, F = Bl i   (ideal transformer, ratio 1/Bl)
//   cone:   U = Sd u,  F = Sd p               (ideal transformer, ratio Sd)
//
// and makes each element a two-terminal part to ground at the node it shares
// a velocity with:
//
//   node "u" (cone velocity, m/s):  mass Mms -> capacitor Mms (F)
//                                   compliance Cms -> inductor Cms (H)
//                                   loss Rms (N s/m) -> resistor 1/Rms
//   node "q" (volume velocity, m^3/s): box air Cab = Vb/(rho c^2) -> inductor Cab,
//                                   box absorption R_ab (series with Cab in the
//                                   impedance analogy) -> resistor 1/R_ab here;
//                                   then, in series (it's in parallel with the box air in
//                                   the impedance analogy): the back opening's air plug,
//                                   mass M_p -> capacitor M_p, radiation R_p -> resistor 1/R_p
//
// Mms follows the datasheet convention (it includes the air load on the cone);
// radiation resistance is negligible for the motion at these frequencies.
// Radiation to a microphone happens in the IR stage, from the cone velocity.
#pragma once

#include "../Circuit.h"

#include <cmath>
#include <map>
#include <string>

namespace cd {

struct DriverParams {
    // a generic 12" guitar speaker, typical of published data:
    // Fs ~80 Hz, Qts ~0.45, Vas ~71 L
    double re = 6.4, le = 0.4e-3, l2 = 0.8e-3, r2 = 12;   // coil: Re, Le, semi-inductance L2 || R2
    double bl = 12;                                      // T m
    double mms = 0.022, cms = 1.8e-4, rms = 1.84;        // kg, m/N, N s/m
    double sd = 0.053;                                   // m^2 (12": effective diameter ~26 cm)
};

struct BoxParams {
    double vb = 0.05;      // m^3 (a 1x12 closed back: ~50 l internal)
    double qa = 20;        // absorption Q at the closed-box resonance (unfilled ~50-100, stuffed ~5-10)
    double open = 0;       // back opening area (m^2): 0 closed back, up to the whole back panel
    double baffle = 0.45;  // the box's front (square, m); depth follows from the volume
    double panel = 0.018;  // back panel thickness (m)
};

inline constexpr double kRho = 1.204, kC = 343.0;   // air, 20 C

// Thiele-Small figures from the physical parameters
struct ThieleSmall {
    double fs, qes, qms, qts, vas;
};
inline ThieleSmall thieleSmall(const DriverParams& d)
{
    const double ws = 1 / std::sqrt(d.mms * d.cms);
    ThieleSmall t;
    t.fs = ws / (2 * M_PI);
    t.qes = ws * d.mms * d.re / (d.bl * d.bl);
    t.qms = ws * d.mms / d.rms;
    t.qts = t.qes * t.qms / (t.qes + t.qms);
    t.vas = kRho * kC * kC * d.sd * d.sd * d.cms;
    return t;
}

// The back opening is a plug of air: acoustic mass rho (panel + end corrections) / S
// (0.85 r at each flanged end), in parallel with the box air. A closed back is the
// same circuit with the plug made (effectively) infinitely heavy.
inline constexpr double kClosedMass = 1e6;   // kg/m^4: far above any real opening's
inline double openingMass(const BoxParams& b)
{
    if (b.open <= 0) return kClosedMass;
    const double ro = std::sqrt(b.open / M_PI);
    return std::min(kClosedMass, kRho * (b.panel + 1.7 * ro) / b.open);
}
// Helmholtz resonance of the box air against the opening's plug
inline double helmholtzHz(const BoxParams& b)
{
    return 1 / (2 * M_PI * std::sqrt(openingMass(b) * b.vb / (kRho * kC * kC)));
}
// the opening's radiation resistance for the circuit (rho w^2 / 2 pi c, a constant at
// the Helmholtz frequency; the IR side uses the exact frequency-dependent one)
inline double openingResistance(const BoxParams& b)
{
    const double w = 2 * M_PI * helmholtzHz(b);
    return kRho * w * w / (2 * M_PI * kC);
}

// Closed box alignment (lossless box, ideal amp, Le ignored): system resonance and Q
inline double closedBoxFc(const DriverParams& d, const BoxParams& b)
{
    const auto t = thieleSmall(d);
    return t.fs * std::sqrt(1 + t.vas / b.vb);
}
inline double closedBoxQtc(const DriverParams& d, const BoxParams& b)
{
    const auto t = thieleSmall(d);
    return t.qts * std::sqrt(1 + t.vas / b.vb);
}

struct SpeakerBox {
    net::Circuit c;
    int input = -1;              // amp voltage
    int velocity = -1;           // output: cone velocity (m/s)
    int current = -1;            // output: voice coil current (A)
    std::map<std::string, int> el;
    double rAmp = 0.05;          // solid-state amp output resistance

    void build(const DriverParams& d, const BoxParams& b)
    {
        auto n = [&](const char* s) { return c.node(s); };
        const int amp = n("amp"), vc = n("vc"), le = n("le"), coil = n("coil"), u = n("u"), q = n("q");
        input = c.source(amp, net::GND, rAmp);
        el["re"] = c.resistor(amp, vc, d.re);
        el["le"] = c.inductor(vc, le, d.le);
        el["l2"] = c.inductor(le, coil, d.l2);
        el["r2"] = c.resistor(le, coil, d.r2);
        c.idealTransformer(coil, net::GND, u, net::GND, 1 / d.bl);    // motor (gyrator in mobility form)
        el["mms"] = c.capacitor(u, net::GND, d.mms);
        el["cms"] = c.inductor(u, net::GND, d.cms);
        el["rms"] = c.resistor(u, net::GND, 1 / d.rms);
        c.idealTransformer(u, net::GND, q, net::GND, d.sd);          // cone area
        const double cab = b.vb / (kRho * kC * kC);
        const double wc = 2 * M_PI * closedBoxFc(d, b);
        const int mb = n("mb");
        el["cab"] = c.inductor(q, mb, cab);
        el["rab"] = c.resistor(q, mb, wc * cab * b.qa);              // 1 / R_ab, R_ab = 1 / (wc Cab Qa)
        el["mp"] = c.capacitor(mb, net::GND, openingMass(b));         // the back opening's air plug
        el["rp"] = c.resistor(mb, net::GND, 1 / openingResistance(b));
        velocity = c.output(u);
        current = c.output(amp, vc, 1 / d.re);
    }
};

} // namespace cd
