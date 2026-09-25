// FuzzFaceDK.h -- realtime solver for the fuzz circuit, nodal DK method.
//
// Same discretisation as the reference MNA solver (FuzzFace.h: trapezoidal
// companions, the shared bjt:: model, pnjlim, same Newton steps), rearranged so
// the per-sample work is tiny:
//
//   node voltages  x = D z,   z = [ sigma (reactive state, 6) | u (vcc, vin) | i (4 device currents) ]
//   junctions      v = Ev [s;u] + K i(v)          (4 unknowns: vbe/vbc of Q1, Q2)
//
// D = A^-1 [B_s B_u N_i] is precomputed (in double) whenever a knob moves, so a
// sample is: a few small matrix-vector products plus Newton on a 4x4 system,
// instead of an 11x11 solve per iteration. Each Newton step here is the same
// linear step the MNA solver takes, so the two agree to within the Newton
// tolerance (tools/ff_render --solver compares them).
//
// Real = double (desktop) or float (Cortex-M7 targets: Daisy, Teensy). The
// precompute stays in double either way, so extreme values (1 TOhm "healthy"
// leakage next to kOhm parts) never enter float arithmetic. Header-only, no
// heap, no JUCE.
//
// Reactive state is the trapezoidal history current s = geq v + i, stored
// scaled to volts for capacitors (sigma = s / geq = v + i / geq) so float never
// has to recover a small cap current from the difference of two large numbers.
// The update is then sigma' = 2 v' - sigma (caps) / s' = 2 geq v' + s (inductor).
#pragma once

#include "FuzzFace.h"

namespace cd {

template <typename Real>
class FuzzFaceDKT {
public:
    using Node = FuzzFace::Node;
    static constexpr int N = FuzzFace::N;
    static constexpr int NS = FuzzFace::kReactive;  // capacitor/inductor histories
    static constexpr int NU = 2;                    // vcc, vin
    static constexpr int NI = 4;                    // ic1 ib1 ic2 ib2 == junctions vbe1 vbc1 vbe2 vbc2
    static constexpr int NW = NS + NU;
    static constexpr int NZ = NW + NI;

    int maxIterations = 8;
    Real reltol = Real(1e-3);   // Newton stop: |dv| < 1e-6 + reltol |v| (SPICE default)
    int lastIterations = 0;
    long failures = 0;

    void prepare(double sampleRate)
    {
        ref.prepare(sampleRate);   // sets the step, builds the network, solves DC
        build();
        loadState();
    }

    void setParams(const FuzzFaceParams& p)
    {
        ref.setParams(p);
        build();
    }

    const FuzzFaceParams& params() const { return ref.params(); }

    // DC operating point, via the reference solver (source-stepped Newton).
    void reset()
    {
        ref.reset();
        loadState();
    }

    // One sample at the oversampled rate. vin = pickup EMF in volts.
    // Returns v(out) in volts.
    Real process(Real vin)
    {
        Real w[NW];
        for (int k = 0; k < NS; ++k) w[k] = sig[k];
        w[NS] = vcc;
        w[NS + 1] = vin;

        Real p[NI];
        for (int r = 0; r < NI; ++r) {
            Real acc = 0;
            for (int c = 0; c < NW; ++c) acc += Ev[r][c] * w[c];
            p[r] = acc;
        }

        Real v[NI], i[NI];
        for (int k = 0; k < NI; ++k) v[k] = vj[k];
        lastIterations = newton(p, v, i);

        Real z[NZ];
        for (int k = 0; k < NW; ++k) z[k] = w[k];
        for (int k = 0; k < NI; ++k) z[NW + k] = i[k];

        Real y = 0;
        for (int c = 0; c < NZ; ++c) y += Cout[c] * z[c];
        if (!std::isfinite(y)) {   // blew up: keep the last good state
            ++failures;
            return yLast;
        }

        for (int k = 0; k < NS; ++k) {
            Real acc = 0;
            for (int c = 0; c < NZ; ++c) acc += R[k][c] * z[c];
            vr[k] = acc;
            sig[k] = a2[k] * acc + sgn[k] * sig[k];
        }
        for (int k = 0; k < NI; ++k) vj[k] = v[k];
        for (int k = 0; k < NZ; ++k) zLast[k] = z[k];
        haveZ = true;
        yLast = y;
        return y;
    }

    // Any node voltage after the last sample (for telemetry; costs NZ MACs).
    double node(Node n) const
    {
        if (!haveZ) return dcNodes[n];
        double acc = 0;
        for (int c = 0; c < NZ; ++c) acc += D[n][c] * (double) zLast[c];
        return acc;
    }

private:
    FuzzFace ref;   // topology, device models, DC operating point, reference maths

    // precomputed per parameter set (double -> Real)
    Real Ev[NI][NW] {}, K[NI][NI] {}, R[NS][NZ] {}, Cout[NZ] {};
    Real a2[NS] {}, sgn[NS] {};   // state update: sigma' = a2 v' + sgn sigma
    double scale[NS] {};           // s = scale * sigma (geq for caps, 1 for the inductor)
    double geqD[NS] {};
    double D[N][NZ] {};
    bjt::Model<Real> m[2];
    Real vcc = 9;

    // state
    Real sig[NS] {};             // scaled trapezoidal history (see top)
    Real vr[NS] {};              // reactive element voltages after the last sample
    bool haveState = false;
    Real vj[NI] {};              // limited junction voltages (SPICE-style Newton state)
    Real zLast[NZ] {};
    Real yLast = 0;
    bool haveZ = false;
    double dcNodes[N] {};

    // Newton on v = p + K i(v): same linear step as the MNA solver, with pnjlim.
    // On return v holds the limited junction state and i the device currents
    // linearised at it (what the MNA solver's node solution corresponds to).
    int newton(const Real* p, Real* v, Real* i) const
    {
        Real vxPrev[NI];
        for (int k = 0; k < NI; ++k) vxPrev[k] = v[k];
        int it = 0;
        while (it < maxIterations) {
            ++it;
            bjt::Eval<Real> e[2] = { bjt::evaluate(m[0], v[0], v[1]), bjt::evaluate(m[1], v[2], v[3]) };
            const Real i0[NI] = { e[0].ic, e[0].ib, e[1].ic, e[1].ib };
            // block-diagonal device Jacobian: d(ic, ib)/d(vbe, vbc) per transistor
            Real G[NI][NI] {};
            for (int q = 0; q < 2; ++q) {
                G[2 * q][2 * q] = e[q].dicVbe;
                G[2 * q][2 * q + 1] = e[q].dicVbc;
                G[2 * q + 1][2 * q] = e[q].dibVbe;
                G[2 * q + 1][2 * q + 1] = e[q].dibVbc;
            }
            // J = K G - I,  F = p + K i0 - v,  J d = -F
            Real J[NI][NI], d[NI];
            for (int r = 0; r < NI; ++r) {
                Real f = p[r] - v[r];
                for (int c = 0; c < NI; ++c) {
                    Real acc = 0;
                    for (int k = 0; k < NI; ++k) acc += K[r][k] * G[k][c];
                    J[r][c] = acc - (r == c ? Real(1) : Real(0));
                    f += K[r][c] * i0[c];
                }
                d[r] = -f;
            }
            if (!solve4(J, d)) { for (int k = 0; k < NI; ++k) i[k] = Real(NAN); return it; }

            Real conv = 0;
            bool limited = false;
            for (int k = 0; k < NI; ++k) {
                const Real vx = v[k] + d[k];                  // unlimited new junction voltage
                Real lin = i0[k];                             // device currents at vx (linearised)
                for (int c = 0; c < NI; ++c) lin += G[k][c] * d[c];
                i[k] = lin;
                conv = std::max(conv, std::abs(vx - vxPrev[k]) / (Real(1e-6) + reltol * std::abs(vx)));
                vxPrev[k] = vx;
                Real vl = vx;
                limited |= bjt::pnjlim(vl, v[k], m[k / 2].vt, m[k / 2].vcrit);
                v[k] = vl;
            }
            if (conv < 1 && !limited) break;
        }
        return it;
    }

    static bool solve4(Real (&A)[NI][NI], Real (&b)[NI])
    {
        for (int k = 0; k < NI; ++k) {
            int piv = k;
            for (int r = k + 1; r < NI; ++r)
                if (std::abs(A[r][k]) > std::abs(A[piv][k])) piv = r;
            if (!(std::abs(A[piv][k]) > Real(0))) return false;
            if (piv != k) {
                for (int c = 0; c < NI; ++c) std::swap(A[k][c], A[piv][c]);
                std::swap(b[k], b[piv]);
            }
            const Real inv = Real(1) / A[k][k];
            for (int r = k + 1; r < NI; ++r) {
                const Real f = A[r][k] * inv;
                for (int c = k + 1; c < NI; ++c) A[r][c] -= f * A[k][c];
                b[r] -= f * b[k];
            }
        }
        for (int k = NI - 1; k >= 0; --k) {
            Real acc = b[k];
            for (int c = k + 1; c < NI; ++c) acc -= A[k][c] * b[c];
            b[k] = acc / A[k][k];
        }
        return true;
    }

    // D = A^-1 [B_s B_u N_i], then the reduced matrices. All double.
    void build()
    {
        const auto& p = ref.params();
        double A[N][N];
        for (int r = 0; r < N; ++r)
            for (int c = 0; c < N; ++c) A[r][c] = ref.linearMatrix()[r * N + c];

        double B[N][NZ] {};
        auto put = [&](int node, int col, double v) { if (node != FuzzFace::GND) B[node][col] += v; };
        double oldS[NS];   // history currents under the old parameters, to carry the state over
        for (int k = 0; k < NS; ++k) oldS[k] = scale[k] * (double) sig[k];
        const bool carry = haveState;
        double oldGeq[NS];
        for (int k = 0; k < NS; ++k) oldGeq[k] = geqD[k];

        for (int k = 0; k < NS; ++k) {
            const auto e = ref.reactive(k);
            // history current injected as in FuzzFace::process: caps +s at a,
            // -s at b; the inductor the other way round. Columns are scaled so
            // the state variable is sigma = s / scale.
            scale[k] = e.inductor ? 1.0 : e.geq;
            const double sign = (e.inductor ? -1.0 : 1.0) * scale[k];
            put(e.a, k, sign);
            put(e.b, k, -sign);
            geqD[k] = e.geq;
            a2[k] = (Real) (2.0 * e.geq / scale[k]);
            sgn[k] = (Real) (e.inductor ? 1.0 : -1.0);
        }
        if (carry) {
            // same (v, i) as the reference solver keeps across a parameter
            // change: i = s_old - geq_old v, then s_new = geq_new v + i
            for (int k = 0; k < NS; ++k) {
                const double v = (double) vr[k];
                const double s = geqD[k] * v + (oldS[k] - oldGeq[k] * v);
                sig[k] = (Real) (s / scale[k]);
            }
        }
        put(FuzzFace::VP, NS, 1.0 / p.rbat);       // battery Norton source
        put(FuzzFace::P1, NS + 1, 1.0 / p.rsrc);   // pickup Norton source
        for (int q = 0; q < 2; ++q) {
            const auto t = ref.transistor(q);
            // device current leaving a node into the device = -i on that node's KCL
            put(t.c, NW + 2 * q, -1.0);
            put(t.e, NW + 2 * q, 1.0);
            put(t.b, NW + 2 * q + 1, -1.0);
            put(t.e, NW + 2 * q + 1, 1.0);
            const auto& md = t.model;
            m[q] = { (Real) md.bf, (Real) md.is, (Real) md.ise, (Real) md.vt, (Real) md.vcrit, (Real) md.vaf };
        }
        vcc = (Real) p.vcc;

        // LU with partial pivoting, then solve all NZ columns
        int perm[N];
        for (int k = 0; k < N; ++k) perm[k] = k;
        for (int k = 0; k < N; ++k) {
            int piv = k;
            for (int r = k + 1; r < N; ++r)
                if (std::abs(A[r][k]) > std::abs(A[piv][k])) piv = r;
            if (piv != k) {
                for (int c = 0; c < N; ++c) std::swap(A[k][c], A[piv][c]);
                std::swap(perm[k], perm[piv]);
            }
            for (int r = k + 1; r < N; ++r) {
                A[r][k] /= A[k][k];
                for (int c = k + 1; c < N; ++c) A[r][c] -= A[r][k] * A[k][c];
            }
        }
        for (int col = 0; col < NZ; ++col) {
            double y[N];
            for (int r = 0; r < N; ++r) {
                double acc = B[perm[r]][col];
                for (int c = 0; c < r; ++c) acc -= A[r][c] * y[c];
                y[r] = acc;
            }
            for (int r = N - 1; r >= 0; --r) {
                double acc = y[r];
                for (int c = r + 1; c < N; ++c) acc -= A[r][c] * D[c][col];
                D[r][col] = acc / A[r][r];
            }
        }

        auto x = [&](int node, int col) { return node == FuzzFace::GND ? 0.0 : D[node][col]; };
        for (int col = 0; col < NZ; ++col) {
            for (int q = 0; q < 2; ++q) {
                const auto t = ref.transistor(q);
                const double vbe = x(t.b, col) - x(t.e, col);
                const double vbc = x(t.b, col) - x(t.c, col);
                if (col < NW) { Ev[2 * q][col] = (Real) vbe; Ev[2 * q + 1][col] = (Real) vbc; }
                else { K[2 * q][col - NW] = (Real) vbe; K[2 * q + 1][col - NW] = (Real) vbc; }
            }
            for (int k = 0; k < NS; ++k) {
                const auto e = ref.reactive(k);
                R[k][col] = (Real) (x(e.a, col) - x(e.b, col));
            }
            Cout[col] = (Real) D[FuzzFace::OUT][col];
        }
    }

    // pick up the reference solver's state (after its DC solve)
    void loadState()
    {
        for (int k = 0; k < NS; ++k) {
            const auto e = ref.reactive(k);
            vr[k] = (Real) e.vPrev;
            sig[k] = (Real) ((e.geq * e.vPrev + e.iPrev) / scale[k]);
        }
        haveState = true;
        for (int q = 0; q < 2; ++q) {
            const auto t = ref.transistor(q);
            vj[2 * q] = (Real) t.vbe;
            vj[2 * q + 1] = (Real) t.vbc;
        }
        for (int n = 0; n < N; ++n) dcNodes[n] = ref.node((Node) n);
        yLast = (Real) dcNodes[FuzzFace::OUT];
        haveZ = false;
    }
};

using FuzzFaceDK = FuzzFaceDKT<double>;
using FuzzFaceDKf = FuzzFaceDKT<float>;

} // namespace cd
