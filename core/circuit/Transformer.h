// Transformer.h -- realtime audio transformer with a hysteretic core.
//
// Same circuit as sim/transformer/xfmr_core.cir (the spec): a winding pair
// referred to the primary, with the magnetizing branch replaced by a
// Jiles-Atherton core. Topology (referred values: secondary R/n^2, L/n^2, C*n^2):
//
//   vin --(rsrc+rp)-- b1 --Ll1-- m --Ll2'-- b2 --Rs'-- s --+-- Cs' || Rload' || (Rz'+Cz')
//                                |                         |
//                              core                      out = n * v(s)
//
// Solved like the fuzz: the linear network is folded into precomputed
// matrices (nodal DK, trapezoidal companions), leaving one nonlinear port --
// the magnetizing current the core draws at node m -- so Newton is scalar.
// The core itself is a per-sample state update:
//   lam  += T/2 (v + v_prev)                      flux linkage (trapezoidal)
//   B     = lam / (np * ac)
//   M    += dM/dB(B, M, delta) * dB               Jiles-Atherton, B-driven
//   H     = B / mu0 - M,   i = H * le / np
// Header-only, no heap, no JUCE.
#pragma once

#include <algorithm>
#include <cmath>

namespace cd {

struct TransformerParams {
    // winding pair (turns ratio secondary/primary), source, windings, leakage
    double n = 10, rsrc = 150, rp = 30, rs = 2500, ll1 = 0.2e-3, ll2 = 20e-3;
    // secondary shunt capacitance, load, damping (Zobel) network
    double cs = 100e-12, rload = 150e3, rz = 100e3, cz = 100e-12;
    // core geometry: primary turns, cross-section (m^2), magnetic path (m)
    double np = 1000, ac = 0.5e-4, le = 0.05;
    // Jiles-Atherton: saturation (A/m), shape a (A/m), coupling, pinning k (A/m), reversibility
    double ms = 6.4e5, a = 8.5, alpha = 1e-5, k = 4, c = 0.2;
    // A/B reference: an ideal linear magnetizing inductance instead of the core
    bool linearCore = false;
    double lm = 8;
};

// Jiles-Atherton core, B-driven ("inverse" form) with the delta_M correction.
struct JilesAtherton {
    static constexpr double mu0 = 1.25663706e-6;
    double ms, a, alpha, k, c;

    static double langevin(double x)
    {
        return std::abs(x) < 1e-4 ? x / 3 : 1 / std::tanh(x) - 1 / x;
    }
    static double dlangevin(double x)
    {
        if (std::abs(x) < 1e-4) return 1.0 / 3 - x * x / 15;
        const double s = std::sinh(x);
        return 1 / (x * x) - 1 / (s * s);
    }

    // dM/dB at state (B, M) moving in direction delta (+1 / -1)
    double dMdB(double B, double M, double delta) const
    {
        const double H = B / mu0 - M;
        const double x = std::clamp((H + alpha * M) / a, -80.0, 80.0);
        const double man = ms * langevin(x);
        const double dman = ms / a * dlangevin(x);
        const double diff = man - M;
        const double dM = diff * delta > 0 ? 1.0 : 0.0;
        const double dMdH = ((1 - c) * dM * diff / ((1 - c) * delta * k - alpha * diff) + c * dman)
                            / (1 - c * alpha * dman);
        return dMdH / (mu0 * (1 + dMdH));
    }
};

class Transformer {
public:
    // nodes of the referred circuit (ground implicit)
    enum Node { B1, M, B2, S, Z, N };
    static constexpr int NS = 4;           // reactive histories: Ll1, Ll2', Cs', Cz'
    static constexpr int NZ = NS + 2;      // + vin, + core current

    int maxIterations = 16;
    int lastIterations = 0;
    long failures = 0;

    void prepare(double sampleRate)
    {
        dt = 1.0 / sampleRate;
        build();
        reset();
    }

    void setParams(const TransformerParams& np)
    {
        p = np;
        build();
    }

    const TransformerParams& params() const { return p; }

    // demagnetised core, everything at rest
    void reset()
    {
        for (auto& v : vr) v = 0;
        for (auto& v : ir) v = 0;
        vm = lam = B = Mag = 0;
        yLast = 0;
    }

    // one sample: vin = source EMF (V), returns the secondary voltage (V)
    double process(double vin)
    {
        double w[NS + 1];
        for (int k = 0; k < NS; ++k) w[k] = geq[k] * vr[k] + ir[k];
        w[NS] = vin;
        double pm = 0;
        for (int c = 0; c <= NS; ++c) pm += D[M][c] * w[c];

        // scalar Newton on v(m) = pm + K * i_core(v(m))
        const double K = D[M][NZ - 1];
        const double dBdv = dt / (2 * p.np * p.ac);
        const double lenFactor = p.le / p.np;
        double v = vm, i = 0, lamN = lam, BN = B, MN = Mag;
        int it = 0;
        while (it < maxIterations) {
            ++it;
            lamN = lam + 0.5 * dt * (v + vm);
            BN = lamN / (p.np * p.ac);
            double di;
            if (p.linearCore) {
                i = lamN / p.lm;
                di = 0.5 * dt / p.lm;
            } else {
                const double dB = BN - B;
                const double delta = dB > 0 ? 1.0 : (dB < 0 ? -1.0 : (vm >= 0 ? 1.0 : -1.0));
                const double slope = core.dMdB(B, Mag, delta);
                MN = Mag + slope * dB;
                i = (BN / JilesAtherton::mu0 - MN) * lenFactor;
                di = (1 / JilesAtherton::mu0 - slope) * dBdv * lenFactor;
            }
            const double f = pm + K * i - v;
            const double step = -f / (K * di - 1);
            v += step;
            if (std::abs(step) < 1e-9 + 1e-7 * std::abs(v)) break;
        }
        lastIterations = it;
        // finalise the core state at the converged voltage
        lamN = lam + 0.5 * dt * (v + vm);
        BN = lamN / (p.np * p.ac);
        if (p.linearCore) {
            i = lamN / p.lm;
        } else {
            const double dB = BN - B;
            const double delta = dB > 0 ? 1.0 : (dB < 0 ? -1.0 : (vm >= 0 ? 1.0 : -1.0));
            MN = Mag + core.dMdB(B, Mag, delta) * dB;
            i = (BN / JilesAtherton::mu0 - MN) * lenFactor;
        }

        double z[NZ];
        for (int k = 0; k <= NS; ++k) z[k] = w[k];
        z[NZ - 1] = i;
        double y = 0;
        for (int c = 0; c < NZ; ++c) y += D[S][c] * z[c];
        if (!std::isfinite(y)) { ++failures; return yLast; }

        for (int k = 0; k < NS; ++k) {
            double acc = 0;
            for (int c = 0; c < NZ; ++c) acc += R[k][c] * z[c];
            ir[k] = geq[k] * acc + sgn[k] * w[k];   // cap: geq v - s, inductor: geq v + s
            vr[k] = acc;
        }
        vm = v;
        lam = lamN;
        B = BN;
        Mag = MN;
        yLast = p.n * y;
        return yLast;
    }

    // core state for plots / UI
    double fluxDensity() const { return B; }
    double field() const { return B / JilesAtherton::mu0 - Mag; }

private:
    TransformerParams p;
    JilesAtherton core { 6.4e5, 8.5, 1e-5, 4, 0.2 };
    double dt = 1.0 / 192000.0;

    double D[N][NZ] {};        // node voltages = D z
    double R[NS][NZ] {};       // reactive element voltages = R z
    double geq[NS] {}, sgn[NS] {};

    double vr[NS] {}, ir[NS] {};
    double vm = 0, lam = 0, B = 0, Mag = 0, yLast = 0;

    struct Reactive { int a, b; bool inductor; double value; };

    void build()
    {
        core = { p.ms, p.a, p.alpha, p.k, p.c };
        const double n2 = p.n * p.n;
        const Reactive rx[NS] = {
            { B1, M, true, p.ll1 },
            { M, B2, true, p.ll2 / n2 },
            { S, -1, false, p.cs * n2 },
            { Z, -1, false, p.cz * n2 },
        };

        double A[N][N] {};
        auto g = [&](int a, int b, double v) {
            if (a >= 0) A[a][a] += v;
            if (b >= 0) A[b][b] += v;
            if (a >= 0 && b >= 0) { A[a][b] -= v; A[b][a] -= v; }
        };
        g(B1, -1, 1 / (p.rsrc + p.rp));   // source Norton conductance
        g(B2, S, n2 / p.rs);
        g(S, -1, n2 / p.rload);
        g(S, Z, n2 / p.rz);

        double Bm[N][NZ] {};
        auto put = [&](int node, int col, double v) { if (node >= 0) Bm[node][col] += v; };
        for (int k = 0; k < NS; ++k) {
            const auto& e = rx[k];
            geq[k] = e.inductor ? dt / (2 * e.value) : 2 * e.value / dt;
            sgn[k] = e.inductor ? 1.0 : -1.0;
            g(e.a, e.b, geq[k]);
            // history current injection: caps +s at a, inductors the other way round
            const double sign = e.inductor ? -1.0 : 1.0;
            put(e.a, k, sign);
            put(e.b, k, -sign);
        }
        put(B1, NS, 1 / (p.rsrc + p.rp));   // vin
        put(M, NZ - 1, -1.0);               // core current leaves node m

        // D = A^-1 Bm (Gaussian elimination, partial pivoting)
        for (int col = 0; col < NZ; ++col)
            for (int r = 0; r < N; ++r) D[r][col] = Bm[r][col];
        for (int kk = 0; kk < N; ++kk) {
            int piv = kk;
            for (int r = kk + 1; r < N; ++r)
                if (std::abs(A[r][kk]) > std::abs(A[piv][kk])) piv = r;
            if (piv != kk) {
                std::swap(A[kk], A[piv]);
                std::swap(D[kk], D[piv]);
            }
            for (int r = 0; r < N; ++r) {
                if (r == kk) continue;
                const double f = A[r][kk] / A[kk][kk];
                for (int c = 0; c < N; ++c) A[r][c] -= f * A[kk][c];
                for (int c = 0; c < NZ; ++c) D[r][c] -= f * D[kk][c];
            }
        }
        for (int r = 0; r < N; ++r) {
            const double inv = 1 / A[r][r];
            for (int c = 0; c < NZ; ++c) D[r][c] *= inv;
        }
        for (int k = 0; k < NS; ++k)
            for (int c = 0; c < NZ; ++c)
                R[k][c] = (rx[k].a >= 0 ? D[rx[k].a][c] : 0) - (rx[k].b >= 0 ? D[rx[k].b][c] : 0);
    }
};

} // namespace cd
