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
//                                   impedance analogy) -> resistor 1/R_ab here
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
    double vb = 0.05;    // m^3 (a 1x12 closed back: ~50 l internal)
    double qa = 20;      // absorption Q at the closed-box resonance (unfilled ~50-100, stuffed ~5-10)
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
        el["cab"] = c.inductor(q, net::GND, cab);
        el["rab"] = c.resistor(q, net::GND, wc * cab * b.qa);        // 1 / R_ab, R_ab = 1 / (wc Cab Qa)
        velocity = c.output(u);
        current = c.output(amp, vc, 1 / d.re);
    }
};

} // namespace cd
