// Devices.h -- nonlinear devices for Circuit.h.
//
//   Bjt      Gummel-Poon subset shared with the fuzz (bjt:: in FuzzFace.h)
//   Triode   Koren's phenomenological curve model (12AX7 defaults) + grid current
//   JACore   magnetic core: Jiles-Atherton hysteresis, driven by winding voltage
//
// Port conventions (see Circuit.h Port): list ports in the order given here.
#pragma once

#include "Circuit.h"
#include "FuzzFace.h"

namespace cd::net {

// ports: 0 = (control b-e, current c->e: ic), 1 = (control b-c, current b->e: ib)
struct Bjt : Device {
    bjt::Model<double> m;
    explicit Bjt(const bjt::Model<double>& model) : m(model) {}
    static std::vector<Port> ports(int c, int b, int e) { return { { b, e, c, e }, { b, c, b, e } }; }

    int numPorts() const override { return 2; }
    void eval(const double* v, double* i, double* J) override
    {
        const auto e = bjt::evaluate(m, v[0], v[1]);
        i[0] = e.ic;
        i[1] = e.ib;
        J[0] = e.dicVbe;
        J[1] = e.dicVbc;
        J[2] = e.dibVbe;
        J[3] = e.dibVbc;
    }
    bool limit(double* vnew, const double* vold) override
    {
        bool l = bjt::pnjlim(vnew[0], vold[0], m.vt, m.vcrit);
        l |= bjt::pnjlim(vnew[1], vold[1], m.vt, m.vcrit);
        return l;
    }
};

// Koren triode: E1 = vpk/kp * ln(1 + exp(kp (1/mu + vgk / sqrt(kvb + vpk^2))))
//               Ip = 2 E1^ex / kg1  (E1 > 0, else 0)
// Grid current once the grid goes positive: Ig = kgc * softplus(vgk)^1.5.
// ports: 0 = (control g-k, current p->k: plate), 1 = (control p-k, current g->k: grid)
struct Triode : Device {
    double mu = 100, ex = 1.4, kg1 = 1060, kp = 600, kvb = 300;   // 12AX7 (Koren)
    double kgc = 4e-4, vs = 0.05;                                 // grid conduction
    static std::vector<Port> ports(int plate, int grid, int cathode)
    {
        return { { grid, cathode, plate, cathode }, { plate, cathode, grid, cathode } };
    }

    int numPorts() const override { return 2; }
    void eval(const double* v, double* i, double* J) override
    {
        const double vgk = v[0], vpk = std::max(v[1], 0.0);
        const double s = std::sqrt(kvb + vpk * vpk);
        const double arg = kp * (1 / mu + vgk / s);
        const double sp = arg > 30 ? arg : std::log1p(std::exp(arg));
        const double sig = 1 / (1 + std::exp(-arg));
        const double e1 = vpk / kp * sp;
        if (e1 > 0) {
            const double ip = 2 * std::pow(e1, ex) / kg1;
            const double dip = 2 * ex * std::pow(e1, ex - 1) / kg1;
            i[0] = ip;
            J[0] = dip * vpk * sig / s;                                         // d/dvgk
            J[1] = v[1] > 0 ? dip * (sp / kp - sig * vgk * vpk * vpk / (s * s * s)) : 0;   // d/dvpk
        } else {
            i[0] = 0;
            J[0] = J[1] = 0;
        }
        const double x = vgk / vs;
        const double spg = x > 30 ? vgk : vs * std::log1p(std::exp(x));
        i[1] = kgc * std::pow(spg, 1.5);
        J[2] = 1.5 * kgc * std::sqrt(spg) / (1 + std::exp(-x));
        J[3] = 0;
    }
    bool limit(double* vnew, const double* vold) override
    {
        // keep Newton from leaping across the whole curve in one step
        bool l = false;
        auto clamp = [&](double& vn, double vo, double maxStep) {
            if (std::abs(vn - vo) > maxStep) { vn = vo + std::copysign(maxStep, vn - vo); l = true; }
        };
        clamp(vnew[0], vold[0], 0.5);
        clamp(vnew[1], vold[1], 25.0);
        return l;
    }
};

// Magnetic core on a winding: port 0 = (control: winding voltage a-b,
// current a->b: magnetizing current). Flux linkage integrates the winding
// voltage (trapezoidal), B = lam/(np ac), M follows Jiles-Atherton (B-driven,
// delta_M correction), H = B/mu0 - M. The winding's ampere-turns drive the
// core path and an optional air gap lg: np i = H le + (B/mu0) lg.
// A core carrying DC at the operating point starts on its initial
// magnetization curve (initDC), as it would after a slow power-on.
struct JACore : Device {
    static constexpr double mu0 = 1.25663706e-6;
    double np = 1000, ac = 0.5e-4, le = 0.05, lg = 0;            // turns, m^2, core path m, gap m
    double ms = 6.4e5, a = 8.5, alpha = 1e-5, k = 4, c = 0.2;    // JA
    bool linear = false;
    double lm = 8;                                               // when linear
    static std::vector<Port> ports(int a, int b) { return { { a, b, a, b } }; }

    int numPorts() const override { return 1; }
    void setTimestep(double t) override { dt = t; }
    void reset() override { lam = B = M = vPrev = 0; }

    void eval(const double* v, double* i, double* J) override
    {
        step(v[0], i[0], J[0], nullptr);
    }
    void evalDC(const double* v, double* i, double* J) override
    {
        i[0] = 1e6 * v[0];   // a winding is a short at DC
        J[0] = 1e6;
    }
    void commit(const double* v) override
    {
        double i, di;
        step(v[0], i, di, this);
    }

    // Walk the initial magnetization curve from the demagnetised state until
    // the winding current matches the DC operating point.
    void initDC(const double*, const double* i) override
    {
        reset();
        const double target = i[0];
        if (linear || std::abs(target) < 1e-15) { lam = target * lm; B = lam / (np * ac); return; }
        const double delta = target > 0 ? 1.0 : -1.0;
        double b = 0, mag = 0, prevI = 0;
        const double db = delta * 1e-5;   // tesla per step
        for (int s = 0; s < 400000; ++s) {
            const double nb = b + db, nm = mag + dMdB(b, mag, delta) * db;
            const double ni = current(nb, nm);
            if ((ni - target) * delta >= 0) {   // crossed: interpolate
                const double f = (target - prevI) / (ni - prevI);
                b += f * db;
                mag += f * (nm - mag);
                break;
            }
            b = nb;
            mag = nm;
            prevI = ni;
        }
        B = b;
        M = mag;
        lam = b * np * ac;
    }

    double fluxDensity() const { return B; }
    double field() const { return B / mu0 - M; }

private:
    double current(double b, double mag) const { return ((b / mu0 - mag) * le + b / mu0 * lg) / np; }

    double dt = 1.0 / 192000;
    double lam = 0, B = 0, M = 0, vPrev = 0;

    static double langevin(double x) { return std::abs(x) < 1e-4 ? x / 3 : 1 / std::tanh(x) - 1 / x; }
    static double dlangevin(double x)
    {
        if (std::abs(x) < 1e-4) return 1.0 / 3 - x * x / 15;
        const double sh = std::sinh(x);
        return 1 / (x * x) - 1 / (sh * sh);
    }
    double dMdB(double b, double mag, double delta) const
    {
        const double h = b / mu0 - mag;
        const double x = std::clamp((h + alpha * mag) / a, -80.0, 80.0);
        const double man = ms * langevin(x), dman = ms / a * dlangevin(x);
        const double diff = man - mag;
        const double dM = diff * delta > 0 ? 1.0 : 0.0;
        const double dMdH = ((1 - c) * dM * diff / ((1 - c) * delta * k - alpha * diff) + c * dman)
                            / (1 - c * alpha * dman);
        return dMdH / (mu0 * (1 + dMdH));
    }

    // current and di/dv at winding voltage v; with `into`, also advance the state
    void step(double v, double& i, double& di, JACore* into)
    {
        const double lamN = lam + 0.5 * dt * (v + vPrev);
        const double BN = lamN / (np * ac);
        if (linear) {
            i = lamN / lm;
            di = 0.5 * dt / lm;
            if (into) { lam = lamN; B = BN; vPrev = v; }
            return;
        }
        const double dB = BN - B;
        const double delta = dB > 0 ? 1.0 : (dB < 0 ? -1.0 : (vPrev >= 0 ? 1.0 : -1.0));
        const double slope = dMdB(B, M, delta);
        const double MN = M + slope * dB;
        i = current(BN, MN);
        di = ((1 / mu0 - slope) * le + lg / mu0) * (0.5 * dt / (np * ac)) / np;
        if (into) { lam = lamN; B = BN; M = MN; vPrev = v; }
    }
};

} // namespace cd::net
