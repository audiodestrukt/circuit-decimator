// FuzzFace.h -- realtime fixed-step circuit solver for sim/fuzzface.cir.
//
// Same topology, values and transistor model as the ngspice deck (the deck is
// the spec; sim/compare.py checks this against it). No JUCE dependency.
//
// Method: modified nodal analysis on the 11 non-ground nodes. Batteries and
// the pickup are Norton equivalents, capacitors and the pickup inductor are
// trapezoidal companion models, and the two BJTs are a Gummel-Poon subset
// (IS BF BR VAF ISE NE, XTI/EG temperature scaling) linearised per Newton
// iteration with SPICE's pnjlim junction limiting. Newton warm-starts from the
// previous sample and is capped at maxIterations: when the circuit is being
// "destroyed" hard enough that it won't converge, we keep the best iterate
// instead of stalling the audio thread.
#pragma once

#include <cmath>
#include <algorithm>

namespace cd {

// Fixed-size, int-indexed, copyable storage for the MNA system.
template <int Size>
struct Vec {
    double d[static_cast<unsigned>(Size)] {};
    double& operator[](int i) { return d[i]; }
    double operator[](int i) const { return d[i]; }
    void fill(double v) { for (auto& e : d) e = v; }
};

// Gummel-Poon subset shared by every solver (FuzzFace.h reference MNA,
// FuzzFaceDK.h realtime DK), templated on precision so the float path uses the
// exact same equations. Mirrors ngspice bjtload.c / bjttemp.c for IS BF BR VAF
// ISE NE with XTI/EG temperature scaling (XTB = 0).
namespace bjt {

inline constexpr double BR = 5, NE = 1.5, ISE = 5e-14, XTI = 3, EG = 1.11;
inline constexpr double TNOM = 300.15, KQ = 8.617333262e-5, GMIN = 1e-12;

template <typename R>
struct Model {
    R bf = 0, is = 0, ise = 0, vt = 0, vcrit = 0, vaf = 0;
};

// collector/base currents and their derivatives w.r.t. (vbe, vbc)
template <typename R>
struct Eval {
    R ic, ib, dicVbe, dicVbc, dibVbe, dibVbc;
};

template <typename R>
inline Eval<R> evaluate(const Model<R>& m, R vbe, R vbc)
{
    const R vt = m.vt, gmin = R(GMIN), br = R(BR), ne = R(NE);
    const R evbe = std::exp(std::min(vbe / vt, R(80)));
    const R evbc = std::exp(std::min(vbc / vt, R(80)));
    const R cbe = m.is * (evbe - 1) + gmin * vbe;
    const R gbe = m.is * evbe / vt + gmin;
    const R cbc = m.is * (evbc - 1) + gmin * vbc;
    const R gbc = m.is * evbc / vt + gmin;
    const R eben = std::exp(std::min(vbe / (ne * vt), R(80)));
    const R cben = m.ise * (eben - 1);
    const R gben = m.ise * eben / (ne * vt);

    const R q1 = R(1) / (R(1) - vbc / m.vaf);
    const R qb = q1;
    const R dqbdvc = q1 * qb / m.vaf;

    const R ic = (cbe - cbc) / qb - cbc / br;
    const R ib = cbe / m.bf + cben + cbc / br;
    const R gm = gbe / qb;
    const R go = (gbc + (cbe - cbc) * dqbdvc / qb) / qb;
    const R gpi = gbe / m.bf + gben;
    const R gmu = gbc / br;
    return { ic, ib, gm, -go - gmu, gpi, gmu };
}

// SPICE junction limiting; returns true if vnew had to be limited
template <typename R>
inline bool pnjlim(R& vnew, R vold, R vt, R vcrit)
{
    if (vnew > vcrit && std::abs(vnew - vold) > 2 * vt) {
        if (vold > 0) {
            const R arg = 1 + (vnew - vold) / vt;
            vnew = arg > 0 ? vold + vt * std::log(arg) : vcrit;
        } else {
            vnew = vt * std::log(vnew / vt);
        }
        return true;
    }
    return false;
}

// temperature-scaled model (ngspice bjttemp.c)
inline Model<double> model(double isat, double bf, double vaf, double tempC)
{
    const double T = tempC + 273.15;
    const double vt = T * KQ;
    const double ratio = T / TNOM;
    const double factlog = (ratio - 1) * EG / vt + XTI * std::log(ratio);
    const double is = isat * std::exp(factlog);
    Model<double> m;
    m.bf = bf;
    m.is = is;
    m.ise = ISE * std::exp(factlog / NE);
    m.vt = vt;
    m.vcrit = vt * std::log(vt / (std::sqrt(2.0) * is));
    m.vaf = vaf;
    return m;
}

} // namespace bjt

struct FuzzFaceParams {
    // supply: battery EMF, internal resistance (dying battery), bulk cap
    double vcc = 9, rbat = 1, cbulk = 100e-9;
    // guitar pickup: source R, coil L, pickup+cable C
    double rsrc = 6e3, lpu = 2.5, ccab = 500e-12;
    // input coupling cap and its leakage
    double cin = 2.2e-6, rleakCin = 1e12;
    // bias network (rc2b = Q2 bias trimmer)
    double rc1 = 33e3, rc2a = 470, rc2b = 5.6e3, rfb = 100e3;
    // fuzz pot (0..1) and bypass cap
    double fuzz = 1, rfuzz = 1e3, cfz = 20e-6;
    // output coupling, volume pot (0..1), amp input
    double cout = 10e-9, vol = 0.5, rvol = 500e3, rload = 1e6;
    // transistors
    double bf1 = 250, bf2 = 250, isat = 1e-14, vaf = 80;
    double rleak1 = 1e12, rleak2 = 1e12;
    double tempC = 27;
};

class FuzzFace {
public:
    enum Node { VP, P1, GIN, B1, C1, C2, E2, TAP, FZW, O1, OUT, N };
    static constexpr int GND = -1;

    int maxIterations = 8;

    void prepare(double sampleRate)
    {
        dt = 1.0 / sampleRate;
        setParams(p);
        reset();
    }

    void setParams(const FuzzFaceParams& np)
    {
        p = np;
        buildLinear();
        updateTransistors();
    }

    const FuzzFaceParams& params() const { return p; }

    // DC operating point (caps open, inductor shorted), reached by ramping
    // the battery up so Newton always starts near a solution.
    void reset()
    {
        x.fill(0.0);
        for (auto& q : bjt) q.vbe = q.vbc = 0.0;

        Vec<N * N> G {};
        Vec<N> bConst {};
        for (auto& r : resistors) stampG(G, r.a, r.b, r.g);
        stampG(G, VP, GND, 1.0 / p.rbat);
        stampG(G, P1, GND, 1.0 / p.rsrc);
        stampG(G, inductor.a, inductor.b, 1e3);

        constexpr int steps = 20;
        for (int k = 1; k <= steps; ++k) {
            bConst.fill(0.0);
            bConst[VP] = p.vcc * k / steps / p.rbat;
            newton(G, bConst, 100);
        }

        for (auto& c : caps) { c.vPrev = v(c.a) - v(c.b); c.iPrev = 0.0; }
        inductor.vPrev = v(inductor.a) - v(inductor.b);
        inductor.iPrev = inductor.vPrev * 1e3;
    }

    // One sample at the oversampled rate. vin = pickup EMF in volts.
    // Returns v(out) in volts (DC-coupled, as the circuit produces it).
    double process(double vin)
    {
        Vec<N> b = bLin;
        b[P1] += vin / p.rsrc;
        for (auto& c : caps) {
            c.ieq = c.geq * c.vPrev + c.iPrev;
            inject(b, c.a, c.b, c.ieq);
        }
        inductor.ieq = inductor.iPrev + inductor.geq * inductor.vPrev;
        inject(b, inductor.a, inductor.b, -inductor.ieq);

        const auto xSave = x;
        Bjt jSave[2] = { bjt[0], bjt[1] };
        lastIterations = newton(Glin, b, maxIterations);

        if (!std::isfinite(x[OUT])) {
            // blew up: hold the last good state, skip this sample's update
            x = xSave;
            bjt[0] = jSave[0];
            bjt[1] = jSave[1];
            ++failures;
            return x[OUT];
        }

        for (auto& c : caps) {
            const double vc = v(c.a) - v(c.b);
            c.iPrev = c.geq * vc - c.ieq;
            c.vPrev = vc;
        }
        const double vl = v(inductor.a) - v(inductor.b);
        inductor.iPrev = inductor.geq * vl + inductor.ieq;
        inductor.vPrev = vl;
        return x[OUT];
    }

    double node(Node n) const { return x[n]; }
    int lastIterations = 0;
    long failures = 0;

    // ---- introspection for other solvers (FuzzFaceDK builds its matrices from these)
    // linear conductance matrix incl. reactive companions and source conductances
    const Vec<N * N>& linearMatrix() const { return Glin; }
    // reactive elements 0..4 = capacitors, 5 = pickup inductor
    static constexpr int kReactive = 6;
    struct Reactive { int a, b; double geq, vPrev, iPrev; bool inductor; };
    Reactive reactive(int k) const
    {
        if (k < 5) return { caps[k].a, caps[k].b, caps[k].geq, caps[k].vPrev, caps[k].iPrev, false };
        return { inductor.a, inductor.b, inductor.geq, inductor.vPrev, inductor.iPrev, true };
    }
    struct Transistor { int c, b, e; bjt::Model<double> model; double vbe, vbc; };
    Transistor transistor(int i) const { return { bjt[i].c, bjt[i].b, bjt[i].e, bjt[i].m, bjt[i].vbe, bjt[i].vbc }; }

private:
    struct Res { int a, b; double g; };
    struct Cap { int a, b; double c, geq = 0, ieq = 0, vPrev = 0, iPrev = 0; };
    struct Ind { int a, b; double l, geq = 0, ieq = 0, vPrev = 0, iPrev = 0; };
    struct Bjt {
        int c, b, e;
        bjt::Model<double> m;
        double vbe = 0, vbc = 0;   // limited junction voltages (SPICE-style state)
    };

    FuzzFaceParams p;
    double dt = 1.0 / 192000.0;
    Vec<N> x {};
    Vec<N * N> Glin {};
    Vec<N> bLin {};
    Res resistors[16] {};
    Cap caps[5] {};
    Ind inductor {};
    Bjt bjt[2] {};

    double v(int n) const { return n == GND ? 0.0 : x[n]; }

    static void stampG(Vec<N * N>& G, int a, int b, double g)
    {
        if (a != GND) G[a * N + a] += g;
        if (b != GND) G[b * N + b] += g;
        if (a != GND && b != GND) { G[a * N + b] -= g; G[b * N + a] -= g; }
    }

    // current i injected into node a, drawn from node b
    static void inject(Vec<N>& rhs, int a, int b, double i)
    {
        if (a != GND) rhs[a] += i;
        if (b != GND) rhs[b] -= i;
    }

    static double pot(double frac, double r) { return std::max(frac * r, 1.0); }

    void buildLinear()
    {
        int k = 0;
        auto R = [&](int a, int b, double r) { resistors[k++] = { a, b, 1.0 / r }; };
        R(VP, C1, p.rc1);
        R(GIN, B1, p.rleakCin);
        R(C1, B1, p.rleak1);
        R(C2, C1, p.rleak2);
        R(C2, TAP, p.rc2a);
        R(TAP, VP, p.rc2b);
        R(E2, B1, p.rfb);
        R(E2, FZW, pot(1 - p.fuzz, p.rfuzz));
        R(FZW, GND, pot(p.fuzz, p.rfuzz));
        R(O1, OUT, pot(1 - p.vol, p.rvol));
        R(OUT, GND, pot(p.vol, p.rvol));
        R(OUT, GND, p.rload);
        for (; k < 16; ++k) resistors[k] = { GND, GND, 0.0 };

        auto C = [&](Cap& c, int a, int b, double val) { c.a = a; c.b = b; c.c = val; c.geq = 2 * val / dt; };
        C(caps[0], VP, GND, p.cbulk);
        C(caps[1], GIN, GND, p.ccab);
        C(caps[2], GIN, B1, p.cin);
        C(caps[3], FZW, GND, p.cfz);
        C(caps[4], TAP, O1, p.cout);
        inductor.a = P1; inductor.b = GIN; inductor.l = p.lpu;
        inductor.geq = dt / (2 * p.lpu);

        Glin.fill(0.0);
        for (auto& r : resistors) stampG(Glin, r.a, r.b, r.g);
        for (auto& c : caps) stampG(Glin, c.a, c.b, c.geq);
        stampG(Glin, inductor.a, inductor.b, inductor.geq);
        stampG(Glin, VP, GND, 1.0 / p.rbat);
        stampG(Glin, P1, GND, 1.0 / p.rsrc);

        bLin.fill(0.0);
        bLin[VP] = p.vcc / p.rbat;
    }

    void updateTransistors()
    {
        const double bfs[2] = { p.bf1, p.bf2 };
        bjt[0].c = C1; bjt[0].b = B1; bjt[0].e = GND;
        bjt[1].c = C2; bjt[1].b = C1; bjt[1].e = E2;
        for (int i = 0; i < 2; ++i) bjt[i].m = bjt::model(p.isat, bfs[i], p.vaf, p.tempC);
    }

    // Transistor linearised at its (limited) junction voltages and stamped as a
    // companion into A/rhs.
    void stampBjt(const Bjt& q, Vec<N * N>& A, Vec<N>& rhs) const
    {
        const double vbe = q.vbe, vbc = q.vbc;
        const auto e = bjt::evaluate(q.m, vbe, vbc);

        // terminal current leaving node x: i0 + dVbe*(vB-vE) + dVbc*(vB-vC)
        auto row = [&](int n, double i0, double dVbe, double dVbc) {
            if (n == GND) return;
            if (q.b != GND) A[n * N + q.b] += dVbe + dVbc;
            if (q.e != GND) A[n * N + q.e] -= dVbe;
            if (q.c != GND) A[n * N + q.c] -= dVbc;
            rhs[n] -= i0 - dVbe * vbe - dVbc * vbc;
        };
        row(q.c, e.ic, e.dicVbe, e.dicVbc);
        row(q.b, e.ib, e.dibVbe, e.dibVbc);
        row(q.e, -(e.ic + e.ib), -(e.dicVbe + e.dibVbe), -(e.dicVbc + e.dibVbc));
    }

    // Gaussian elimination with partial pivoting, in place; result in rhs.
    static bool solve(Vec<N * N>& A, Vec<N>& rhs)
    {
        for (int k = 0; k < N; ++k) {
            int piv = k;
            double best = std::abs(A[k * N + k]);
            for (int i = k + 1; i < N; ++i)
                if (std::abs(A[i * N + k]) > best) { best = std::abs(A[i * N + k]); piv = i; }
            if (best < 1e-300) return false;
            if (piv != k) {
                for (int j = k; j < N; ++j) std::swap(A[k * N + j], A[piv * N + j]);
                std::swap(rhs[k], rhs[piv]);
            }
            const double inv = 1.0 / A[k * N + k];
            for (int i = k + 1; i < N; ++i) {
                const double f = A[i * N + k] * inv;
                for (int j = k + 1; j < N; ++j) A[i * N + j] -= f * A[k * N + j];
                rhs[i] -= f * rhs[k];
            }
        }
        for (int k = N - 1; k >= 0; --k) {
            double s = rhs[k];
            for (int j = k + 1; j < N; ++j) s -= A[k * N + j] * rhs[j];
            rhs[k] = s / A[k * N + k];
        }
        return true;
    }

    int newton(const Vec<N * N>& G, const Vec<N>& b, int maxIter)
    {
        int it = 0;
        while (it < maxIter) {
            ++it;
            auto A = G;
            auto rhs = b;
            for (auto& q : bjt) stampBjt(q, A, rhs);
            if (!solve(A, rhs)) { x.fill(NAN); return it; }

            double dx = 0;
            for (int i = 0; i < N; ++i) {
                const double d = std::abs(rhs[i] - x[i]);
                dx = std::max(dx, d / (1e-6 + 1e-3 * std::abs(rhs[i])));
            }
            x = rhs;

            bool limited = false;
            for (auto& q : bjt) {
                double vbe = v(q.b) - v(q.e), vbc = v(q.b) - v(q.c);
                limited |= bjt::pnjlim(vbe, q.vbe, q.m.vt, q.m.vcrit);
                limited |= bjt::pnjlim(vbc, q.vbc, q.m.vt, q.m.vcrit);
                q.vbe = vbe; q.vbc = vbc;
            }
            if (dx < 1.0 && !limited) break;
        }
        return it;
    }
};

} // namespace cd
