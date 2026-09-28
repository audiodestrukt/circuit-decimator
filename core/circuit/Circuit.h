// Circuit.h -- general realtime circuit engine (nodal DK method).
//
// Describe a circuit with resistors, capacitors, inductors, voltage sources,
// ideal transformers and nonlinear devices; the engine derives the realtime
// solver the hand-built ones (FuzzFaceDK.h, Transformer.h) use:
//
//   MNA unknowns x = node voltages + auxiliary currents (voltage sources,
//   ideal transformers). Capacitors and inductors become trapezoidal
//   companions, so for a fixed parameter set the linear part is one matrix A:
//
//     x = D z,   z = [ s (reactive histories) | u (inputs) | i (device port currents) ]
//     D = A^-1 [B_s B_u B_i]
//
//   Device ports are controlled by voltages v = Nv x, so per sample
//     v = Ev [s;u] + K i(v)
//   is solved by Newton on the device ports only (a few unknowns), then the
//   reactive states update from R z. A is rebuilt only when a value changes.
//
// Devices are small classes with a few ports (Devices.h: BJT, triode,
// Jiles-Atherton core). Each port has a controlling voltage (between two
// nodes) and a current it draws (leaving one node, returning at another).
// No heap allocation after build(); no JUCE.
#pragma once

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace cd::net {

constexpr int GND = -1;

// A nonlinear element with n ports. v: controlling voltages, i: port
// currents, J: di/dv (row-major n x n). State (flux, magnetization...) moves
// only in commit(), so eval() can be called many times per sample.
struct Device {
    virtual ~Device() = default;
    virtual int numPorts() const = 0;
    virtual void setTimestep(double) {}
    virtual void eval(const double* v, double* i, double* J) = 0;
    // Newton step limiting (e.g. SPICE pnjlim); return true if anything was limited
    virtual bool limit(double* /*vnew*/, const double* /*vold*/) { return false; }
    virtual void commit(const double* /*v*/) {}
    virtual void reset() {}
    // DC operating point: inductive devices behave as shorts, say
    virtual void evalDC(const double* v, double* i, double* J) { eval(v, i, J); }
    // after the DC solve: the port voltages and currents it found (a core
    // carrying DC current magnetises itself to match, for example)
    virtual void initDC(const double* /*v*/, const double* /*i*/) {}
    // true when, within one sample, the currents are linear in the port voltages
    // (coefficients may change between samples, from state moved in commit()):
    // if every device says so, one Newton step is exact and the solve stops there
    virtual bool linearWithinSample() const { return false; }
};

struct Port {
    int cp, cn;   // controlling voltage v = x[cp] - x[cn]
    int ip, in;   // current i leaves node ip into the device, returns at node in
};

class Circuit {
public:
    // ---- building ------------------------------------------------------------
    int node(const std::string& name)
    {
        if (name == "0" || name == "gnd") return GND;
        for (size_t k = 0; k < names.size(); ++k)
            if (names[k] == name) return (int) k;
        names.push_back(name);
        return (int) names.size() - 1;
    }

    int resistor(int a, int b, double r) { elems.push_back({ R, a, b, r }); return (int) elems.size() - 1; }
    int capacitor(int a, int b, double c) { elems.push_back({ C, a, b, c }); return (int) elems.size() - 1; }
    int inductor(int a, int b, double l) { elems.push_back({ L, a, b, l }); return (int) elems.size() - 1; }

    // Voltage source between a (+) and b (-). With series resistance rs > 0 it
    // is a Norton equivalent (no extra unknown). Returns the input index;
    // set its value with setInput(). dc marks supply rails for source stepping.
    int source(int a, int b, double rs = 0, double value = 0, bool dc = false)
    {
        sources.push_back({ a, b, rs, dc });
        inputs.push_back(value);
        return (int) inputs.size() - 1;
    }

    // Ideal transformer: primary (a, b), secondary (c, d), ratio = Ns / Np
    void idealTransformer(int a, int b, int c, int d, double ratio) { xfmrs.push_back({ a, b, c, d, ratio }); }

    // Nonlinear device with one Port per device port.
    Device& device(std::unique_ptr<Device> dev, std::vector<Port> ports)
    {
        devs.push_back({ std::move(dev), std::move(ports) });
        return *devs.back().dev;
    }

    // Output = gain * (x[a] - x[b]); returns output index
    int output(int a, int b = GND, double gain = 1)
    {
        outs.push_back({ a, b, gain });
        return (int) outs.size() - 1;
    }

    // ---- running ---------------------------------------------------------------
    int maxIterations = 20;
    double reltol = 1e-3;
    bool extrapolate = true;   // Newton initial guess from the last two samples
    int lastIterations = 0;
    long failures = 0;

    void prepare(double sampleRate)
    {
        dt = 1.0 / sampleRate;
        for (auto& d : devs) d.dev->setTimestep(dt);
        allocate();
        build();
        dcOperatingPoint();
    }

    // Change an element's value (resistance, capacitance, inductance) and
    // rebuild the matrices; the reactive state carries over.
    void setValue(int element, double value)
    {
        elems[(size_t) element].value = value;
        build();
    }

    // Batch changes: set values (and transformer ratios) without rebuilding,
    // then rebuildIfDirty() once. No allocation (the matrices are preallocated).
    void setValueDeferred(int element, double value)
    {
        auto& e = elems[(size_t) element].value;
        if (std::abs(e - value) > 0.0) { e = value; dirty = true; }
    }
    double value(int element) const { return elems[(size_t) element].value; }
    int numTransformers() const { return (int) xfmrs.size(); }
    void setRatioDeferred(int transformer, double ratio)
    {
        auto& n = xfmrs[(size_t) transformer].n;
        if (std::abs(n - ratio) > 0.0) { n = ratio; dirty = true; }
    }
    void rebuildIfDirty()
    {
        if (dirty && !A.empty()) { build(); dirty = false; }   // before prepare() there is nothing to rebuild
    }

    void setInput(int k, double value) { inputs[(size_t) k] = value; }
    double getInput(int k) const { return inputs[(size_t) k]; }

    // Everything discharged and demagnetised, as before power is applied (the
    // caller then ramps its supply source up from 0 -- a cold start).
    void zeroState()
    {
        std::fill(vr.begin(), vr.end(), 0.0);
        std::fill(ir.begin(), ir.end(), 0.0);
        std::fill(vj.begin(), vj.end(), 0.0);
        std::fill(vj1.begin(), vj1.end(), 0.0);
        std::fill(z.begin(), z.end(), 0.0);
        for (auto& d : devs) d.dev->reset();
    }

    // Re-solve the DC operating point (a warm start: settled, as if powered on gently).
    void warmStart() { dcOperatingPoint(); }

    // one sample
    void process()
    {
        for (int k = 0; k < NS; ++k) w[(size_t) k] = geq[(size_t) k] * vr[(size_t) k] + ir[(size_t) k];
        for (int k = 0; k < NU; ++k) w[(size_t) (NS + k)] = inputs[(size_t) k];

        for (int r = 0; r < NI; ++r) {
            double acc = 0;
            for (int c = 0; c < NW; ++c) acc += Ev[idx(r, c, NW)] * w[(size_t) c];
            p[(size_t) r] = acc;
        }
        // Newton starts from the last two samples extrapolated: oversampled
        // signals are smooth, so it often lands within tolerance in one step
        for (int k = 0; k < NI; ++k)
            v[(size_t) k] = extrapolate ? 2 * vj[(size_t) k] - vj1[(size_t) k] : vj[(size_t) k];
        if (extrapolate) {   // the devices' own step limits apply to the guess too (pnjlim...)
            int off = 0;
            for (auto& d : devs) {
                d.dev->limit(&v[(size_t) off], &vj[(size_t) off]);
                off += d.dev->numPorts();
            }
        }
        lastIterations = newton(false);

        for (int k = 0; k < NW; ++k) z[(size_t) k] = w[(size_t) k];
        for (int k = 0; k < NI; ++k) z[(size_t) (NW + k)] = cur[(size_t) k];
        bool ok = true;
        for (int k = 0; k < NI; ++k) ok &= std::isfinite(v[(size_t) k]) && std::isfinite(cur[(size_t) k]);
        if (!ok) { ++failures; return; }   // keep the last good state

        for (int k = 0; k < NS; ++k) {
            double acc = 0;
            for (int c = 0; c < NZ; ++c) acc += Rm[idx(k, c, NZ)] * z[(size_t) c];
            ir[(size_t) k] = geq[(size_t) k] * acc + sgn[(size_t) k] * w[(size_t) k];
            vr[(size_t) k] = acc;
        }
        commitDevices();
        for (int k = 0; k < NI; ++k) { vj1[(size_t) k] = vj[(size_t) k]; vj[(size_t) k] = v[(size_t) k]; }
    }

    double out(int k) const
    {
        const auto& o = outs[(size_t) k];
        return o.gain * (x(o.a) - x(o.b));
    }

    // any node voltage after the last sample
    double x(int n) const
    {
        if (n == GND) return 0;
        double acc = 0;
        for (int c = 0; c < NZ; ++c) acc += D[idx(n, c, NZ)] * z[(size_t) c];
        return acc;
    }

    int numNodes() const { return (int) names.size(); }
    int numPorts() const { return NI; }

private:
    enum Kind { R, C, L };
    struct Elem { Kind kind; int a, b; double value; };
    struct Src { int a, b; double rs; bool dc; };
    struct Xfmr { int a, b, c, d; double n; };
    struct Out { int a, b; double gain; };
    struct Dev { std::unique_ptr<Device> dev; std::vector<Port> ports; };

    std::vector<std::string> names;
    std::vector<Elem> elems;
    std::vector<Src> sources;
    std::vector<Xfmr> xfmrs;
    std::vector<Out> outs;
    std::vector<Dev> devs;
    std::vector<double> inputs;
    double dt = 1.0 / 192000;
    bool dirty = false;

    // sizes: nodes, aux unknowns, reactive, inputs, ports
    int NN = 0, NA = 0, NX = 0, NS = 0, NU = 0, NI = 0, NW = 0, NZ = 0;
    std::vector<int> reactive;   // element index of each reactive history
    std::vector<int> auxOfSource;

    std::vector<double> D, Ev, K, Rm, geq, sgn;
    std::vector<double> vr, ir, vj, vj1, w, p, v, cur, z, J, Jg, F, dv, vPrev, A, Bm, xs;
    std::vector<int> blk0, blkN;   // per port: first port and size of its device's block in Jg

    static size_t idx(int r, int c, int cols) { return (size_t) r * (size_t) cols + (size_t) c; }

    void allocate()
    {
        NN = (int) names.size();
        auxOfSource.assign(sources.size(), -1);
        NA = 0;
        for (size_t k = 0; k < sources.size(); ++k)
            if (sources[k].rs <= 0) auxOfSource[k] = NN + NA++;
        NA += (int) xfmrs.size();
        NX = NN + NA;
        reactive.clear();
        for (size_t k = 0; k < elems.size(); ++k)
            if (elems[k].kind != R) reactive.push_back((int) k);
        NS = (int) reactive.size();
        NU = (int) inputs.size();
        NI = 0;
        for (auto& d : devs) NI += d.dev->numPorts();
        NW = NS + NU;
        NZ = NW + NI;
        D.assign((size_t) NX * (size_t) NZ, 0);
        Ev.assign((size_t) NI * (size_t) NW, 0);
        K.assign((size_t) NI * (size_t) NI, 0);
        Rm.assign((size_t) NS * (size_t) NZ, 0);
        geq.assign((size_t) NS, 0.0);
        sgn.assign((size_t) NS, 0.0);
        vr.assign((size_t) NS, 0.0);
        ir.assign((size_t) NS, 0.0);
        vj.assign((size_t) NI, 0.0);
        vj1.assign((size_t) NI, 0.0);
        w.assign((size_t) NW, 0.0);
        p.assign((size_t) NI, 0.0);
        v.assign((size_t) NI, 0.0);
        cur.assign((size_t) NI, 0.0);
        z.assign((size_t) NZ, 0.0);
        J.assign((size_t) NI * (size_t) NI, 0);
        Jg.assign((size_t) NI * (size_t) NI, 0);   // block diagonal: only device blocks are ever written
        blk0.assign((size_t) NI, 0);
        blkN.assign((size_t) NI, 0);
        for (int off = 0; auto& d : devs) {
            const int n = d.dev->numPorts();
            for (int k = 0; k < n; ++k) { blk0[(size_t) (off + k)] = off; blkN[(size_t) (off + k)] = n; }
            off += n;
        }
        F.assign((size_t) NI, 0.0);
        dv.assign((size_t) NI, 0.0);
        vPrev.assign((size_t) NI, 0.0);
        A.assign((size_t) NX * (size_t) NX, 0);
        Bm.assign((size_t) NX * (size_t) NZ, 0);
        xs.assign((size_t) NX, 0.0);
    }

    // Assemble A (and the B columns) for transient (dc = false) or the DC
    // operating point (dc = true: capacitors open, inductors shorted), then
    // D = A^-1 B and the reduced matrices.
    void build(bool dc = false)
    {
        std::fill(A.begin(), A.end(), 0.0);
        std::fill(Bm.begin(), Bm.end(), 0.0);
        auto g = [&](int a, int b, double val) {
            if (a >= 0) A[idx(a, a, NX)] += val;
            if (b >= 0) A[idx(b, b, NX)] += val;
            if (a >= 0 && b >= 0) { A[idx(a, b, NX)] -= val; A[idx(b, a, NX)] -= val; }
        };
        auto put = [&](int row, int col, double val) { if (row >= 0) Bm[idx(row, col, NZ)] += val; };

        for (int n = 0; n < NN; ++n) A[idx(n, n, NX)] += 1e-12;   // gmin: no floating nodes
        for (auto& e : elems)
            if (e.kind == R) g(e.a, e.b, 1 / e.value);
        for (int k = 0; k < NS; ++k) {
            const auto& e = elems[(size_t) reactive[(size_t) k]];
            const bool ind = e.kind == L;
            if (dc) {
                geq[(size_t) k] = ind ? 1e6 : 0;   // short / open
            } else {
                geq[(size_t) k] = ind ? dt / (2 * e.value) : 2 * e.value / dt;
                sgn[(size_t) k] = ind ? 1.0 : -1.0;
            }
            g(e.a, e.b, geq[(size_t) k]);
            if (!dc) {
                // history current: caps +s at a, inductors the other way round
                const double s = ind ? -1.0 : 1.0;
                put(e.a, k, s);
                put(e.b, k, -s);
            }
        }
        for (size_t k = 0; k < sources.size(); ++k) {
            const auto& s = sources[k];
            const int col = NS + (int) k;
            if (s.rs > 0) {               // Norton: conductance + current u / rs
                g(s.a, s.b, 1 / s.rs);
                put(s.a, col, 1 / s.rs);
                put(s.b, col, -1 / s.rs);
            } else {                      // ideal: aux current j (a -> b through the source)
                const int j = auxOfSource[k];
                if (s.a >= 0) { A[idx(s.a, j, NX)] += 1; A[idx(j, s.a, NX)] += 1; }
                if (s.b >= 0) { A[idx(s.b, j, NX)] -= 1; A[idx(j, s.b, NX)] -= 1; }
                put(j, col, 1);
            }
        }
        for (size_t k = 0; k < xfmrs.size(); ++k) {
            // j = current into the secondary winding at c; primary draws n j... with
            // sign so power balances: KCL a: -n j, b: +n j, c: +j, d: -j;
            // row j: (x_c - x_d) - n (x_a - x_b) = 0
            const auto& t = xfmrs[k];
            const int j = NN + (NA - (int) xfmrs.size()) + (int) k;
            auto set = [&](int r, int c, double val) { if (r >= 0 && c >= 0) A[idx(r, c, NX)] += val; };
            set(t.c, j, 1);
            set(t.d, j, -1);
            set(t.a, j, -t.n);
            set(t.b, j, t.n);
            set(j, t.c, 1);
            set(j, t.d, -1);
            set(j, t.a, -t.n);
            set(j, t.b, t.n);
        }
        int col = NW;
        for (auto& d : devs)
            for (auto& pt : d.ports) {
                put(pt.ip, col, -1);   // current leaves ip into the device
                put(pt.in, col, 1);
                ++col;
            }

        solveColumns();

        auto xrow = [&](int n, int c) { return n == GND ? 0.0 : D[idx(n, c, NZ)]; };
        int r = 0;
        for (auto& d : devs)
            for (auto& pt : d.ports) {
                for (int c = 0; c < NZ; ++c) {
                    const double val = xrow(pt.cp, c) - xrow(pt.cn, c);
                    if (c < NW) Ev[idx(r, c, NW)] = val;
                    else K[idx(r, c - NW, NI)] = val;
                }
                ++r;
            }
        for (int k = 0; k < NS; ++k) {
            const auto& e = elems[(size_t) reactive[(size_t) k]];
            for (int c = 0; c < NZ; ++c) Rm[idx(k, c, NZ)] = xrow(e.a, c) - xrow(e.b, c);
        }
    }

    // D = A^-1 Bm, Gauss-Jordan with partial pivoting (A, Bm destroyed)
    void solveColumns()
    {
        for (int k = 0; k < NX; ++k) {
            int piv = k;
            for (int rr = k + 1; rr < NX; ++rr)
                if (std::abs(A[idx(rr, k, NX)]) > std::abs(A[idx(piv, k, NX)])) piv = rr;
            if (piv != k) {
                for (int c = 0; c < NX; ++c) std::swap(A[idx(k, c, NX)], A[idx(piv, c, NX)]);
                for (int c = 0; c < NZ; ++c) std::swap(Bm[idx(k, c, NZ)], Bm[idx(piv, c, NZ)]);
            }
            const double inv = 1 / A[idx(k, k, NX)];
            for (int c = 0; c < NX; ++c) A[idx(k, c, NX)] *= inv;
            for (int c = 0; c < NZ; ++c) Bm[idx(k, c, NZ)] *= inv;
            for (int rr = 0; rr < NX; ++rr) {
                if (rr == k) continue;
                const double f = A[idx(rr, k, NX)];
                for (int c = 0; c < NX; ++c) A[idx(rr, c, NX)] -= f * A[idx(k, c, NX)];
                for (int c = 0; c < NZ; ++c) Bm[idx(rr, c, NZ)] -= f * Bm[idx(k, c, NZ)];
            }
        }
        D = Bm;
    }

    void evalDevices(bool dc)
    {
        int off = 0;
        for (auto& d : devs) {
            const int n = d.dev->numPorts();
            double Jl[16];
            if (dc) d.dev->evalDC(&v[(size_t) off], &cur[(size_t) off], Jl);
            else d.dev->eval(&v[(size_t) off], &cur[(size_t) off], Jl);
            for (int r = 0; r < n; ++r)
                for (int c = 0; c < n; ++c) Jg[idx(off + r, off + c, NI)] = Jl[r * n + c];
            off += n;
        }
    }

    // Newton on v = p + K i(v): the same linear step as a full MNA solve.
    // On return v holds the limited port voltages and cur the port currents
    // linearised at the final step (consistent with the node solution).
    int newton(bool dc)
    {
        bool allLinear = !devs.empty();
        for (auto& d : devs) allLinear = allLinear && d.dev->linearWithinSample();
        for (int k = 0; k < NI; ++k) vPrev[(size_t) k] = v[(size_t) k];
        int it = 0;
        while (it < maxIterations) {
            ++it;
            evalDevices(dc);
            // J = K Jg - I,  F = p + K i - v  (Jg is block diagonal: sum over c's block only)
            for (int r = 0; r < NI; ++r) {
                double f = p[(size_t) r] - v[(size_t) r];
                for (int c = 0; c < NI; ++c) {
                    double acc = 0;
                    const int k0 = blk0[(size_t) c], k1 = k0 + blkN[(size_t) c];
                    for (int k = k0; k < k1; ++k) acc += K[idx(r, k, NI)] * Jg[idx(k, c, NI)];
                    J[idx(r, c, NI)] = acc - (r == c ? 1.0 : 0.0);
                    f += K[idx(r, c, NI)] * cur[(size_t) c];
                }
                F[(size_t) r] = -f;
            }
            if (!solveSmall()) return it;
            double conv = 0;
            for (int k = 0; k < NI; ++k) {
                const double vx = v[(size_t) k] + dv[(size_t) k];
                conv = std::max(conv, std::abs(vx - vPrev[(size_t) k]) / (1e-6 + reltol * std::abs(vx)));
                vPrev[(size_t) k] = vx;
            }
            // linearised currents at the new point
            for (int r = 0; r < NI; ++r) {
                double acc = cur[(size_t) r];
                const int c0 = blk0[(size_t) r], c1 = c0 + blkN[(size_t) r];
                for (int c = c0; c < c1; ++c) acc += Jg[idx(r, c, NI)] * dv[(size_t) c];
                cur[(size_t) r] = acc;
            }
            // step, then let devices limit it
            for (int k = 0; k < NI; ++k) F[(size_t) k] = v[(size_t) k];   // old
            for (int k = 0; k < NI; ++k) v[(size_t) k] += dv[(size_t) k];
            bool limited = false;
            int off = 0;
            for (auto& d : devs) {
                limited |= d.dev->limit(&v[(size_t) off], &F[(size_t) off]);
                off += d.dev->numPorts();
            }
            if (conv < 1 && !limited) break;
            if (allLinear && !dc && !limited) break;   // one step is exact
        }
        return it;
    }

    // J dv = F (NI x NI), partial pivoting; J and F destroyed, result in dv
    bool solveSmall()
    {
        for (int k = 0; k < NI; ++k) {
            int piv = k;
            for (int r = k + 1; r < NI; ++r)
                if (std::abs(J[idx(r, k, NI)]) > std::abs(J[idx(piv, k, NI)])) piv = r;
            if (!(std::abs(J[idx(piv, k, NI)]) > 0)) return false;
            if (piv != k) {
                for (int c = 0; c < NI; ++c) std::swap(J[idx(k, c, NI)], J[idx(piv, c, NI)]);
                std::swap(F[(size_t) k], F[(size_t) piv]);
            }
            for (int r = k + 1; r < NI; ++r) {
                const double f = J[idx(r, k, NI)] / J[idx(k, k, NI)];
                for (int c = k + 1; c < NI; ++c) J[idx(r, c, NI)] -= f * J[idx(k, c, NI)];
                F[(size_t) r] -= f * F[(size_t) k];
            }
        }
        for (int k = NI - 1; k >= 0; --k) {
            double acc = F[(size_t) k];
            for (int c = k + 1; c < NI; ++c) acc -= J[idx(k, c, NI)] * dv[(size_t) c];
            dv[(size_t) k] = acc / J[idx(k, k, NI)];
        }
        return true;
    }

    void commitDevices()
    {
        int off = 0;
        for (auto& d : devs) {
            d.dev->commit(&v[(size_t) off]);
            off += d.dev->numPorts();
        }
    }

    // DC operating point: caps open, inductors shorted, supply rails ramped up
    // (source stepping) so Newton starts near a solution; then the reactive
    // states are loaded from it and the transient matrices are built.
    void dcOperatingPoint()
    {
        for (auto& d : devs) d.dev->reset();
        const std::vector<double> target = inputs;
        build(true);
        std::fill(v.begin(), v.end(), 0.0);
        constexpr int steps = 20;
        for (int s = 1; s <= steps; ++s) {
            for (size_t k = 0; k < sources.size(); ++k)
                inputs[k] = sources[k].dc ? target[k] * s / steps : 0.0;
            for (int k = 0; k < NU; ++k) w[(size_t) (NS + k)] = inputs[(size_t) k];
            for (int r = 0; r < NI; ++r) {
                double acc = 0;
                for (int c = NS; c < NW; ++c) acc += Ev[idx(r, c, NW)] * w[(size_t) c];
                p[(size_t) r] = acc;
            }
            const int saved = maxIterations;
            maxIterations = 200;
            newton(true);
            maxIterations = saved;
        }
        // node solution at the DC point
        for (int k = 0; k < NS; ++k) w[(size_t) k] = 0;
        for (int k = 0; k < NW; ++k) z[(size_t) k] = w[(size_t) k];
        for (int k = 0; k < NI; ++k) z[(size_t) (NW + k)] = cur[(size_t) k];
        for (int n = 0; n < NX; ++n) {
            double acc = 0;
            for (int c = 0; c < NZ; ++c) acc += D[idx(n, c, NZ)] * z[(size_t) c];
            xs[(size_t) n] = acc;
        }
        auto xv = [&](int n) { return n == GND ? 0.0 : xs[(size_t) n]; };
        for (int k = 0; k < NS; ++k) {
            const auto& e = elems[(size_t) reactive[(size_t) k]];
            vr[(size_t) k] = xv(e.a) - xv(e.b);
            ir[(size_t) k] = e.kind == L ? 1e6 * vr[(size_t) k] : 0.0;   // inductor DC current
            if (e.kind == L) vr[(size_t) k] = 0;
        }
        inputs = target;
        {
            int off = 0;
            for (auto& d : devs) {
                d.dev->initDC(&v[(size_t) off], &cur[(size_t) off]);
                off += d.dev->numPorts();
            }
        }
        for (int k = 0; k < NI; ++k) vj[(size_t) k] = vj1[(size_t) k] = v[(size_t) k];
        build(false);
        // z/x reflect the DC point until the first sample
        for (int k = 0; k < NS; ++k) z[(size_t) k] = geq[(size_t) k] * vr[(size_t) k] + ir[(size_t) k];
    }
};

} // namespace cd::net
