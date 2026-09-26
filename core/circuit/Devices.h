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

// softplus log(1 + e^x) and its slope (the logistic), one exp
inline void softplus(double x, double& sp, double& sig)
{
    if (x > 30) { sp = x; sig = 1; return; }
    const double e = std::exp(x);
    sp = std::log1p(e);
    sig = e / (1 + e);
}

// smooth grid conduction kgc * softplus_vs(vgk)^1.5 and its slope
inline void gridCurrent(double vgk, double kgc, double vs, double& i, double& di)
{
    const double x = vgk / vs;
    if (x < -40) { i = di = 0; return; }   // far below conduction: ~1e-26 A
    double sp, sig;
    softplus(x, sp, sig);
    const double spg = vs * sp, r = std::sqrt(spg);
    i = kgc * spg * r;
    di = 1.5 * kgc * r * sig;
}

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
        double sp, sig;
        softplus(arg, sp, sig);
        const double e1 = vpk / kp * sp;
        if (e1 > 0) {
            const double ip = 2 * std::pow(e1, ex) / kg1;
            const double dip = ex * ip / e1;
            i[0] = ip;
            J[0] = dip * vpk * sig / s;                                         // d/dvgk
            J[1] = v[1] > 0 ? dip * (sp / kp - sig * vgk * vpk * vpk / (s * s * s)) : 0;   // d/dvpk
        } else {
            i[0] = 0;
            J[0] = J[1] = 0;
        }
        gridCurrent(vgk, kgc, vs, i[1], J[2]);
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

// Koren pentode (curve model): plate current depends on the grid and screen
// voltages, flattened against plate voltage by atan(vpk/kvb); screen current
// follows the grid + screen/mu drive. Defaults: 6AQ5 (the miniature 6V6),
// calibrated to the datasheet typical point 250 V plate / 250 V screen /
// -12.5 V grid -> 45 mA plate, 4.5 mA screen (tools/pentode_check).
// ports: 0 = (control g1-k, current p->k: plate)
//        1 = (control p-k,  current g2->k: screen)
//        2 = (control g2-k, current g1->k: grid)
struct Pentode : Device {
    double mu = 9.6, ex = 1.35, kg1 = 2436, kg2 = 7488, kp = 40, kvb = 12;
    double kgc = 4e-4, vs = 0.05;
    static std::vector<Port> ports(int plate, int grid, int screen, int cathode)
    {
        return { { grid, cathode, plate, cathode }, { plate, cathode, screen, cathode }, { screen, cathode, grid, cathode } };
    }

    int numPorts() const override { return 3; }
    // J is row-major 3x3: rows = currents (plate, screen, grid), cols = controls (g1k, pk, g2k)
    void eval(const double* v, double* i, double* J) override
    {
        const double vg1 = v[0], vpk = std::max(v[1], 0.0), vg2 = std::max(v[2], 1e-3);
        for (int k = 0; k < 9; ++k) J[k] = 0;
        const double arg = kp * (1 / mu + vg1 / vg2);
        double sp, sig;
        softplus(arg, sp, sig);
        const double e1 = vg2 / kp * sp;
        const double at = std::atan(vpk / kvb), dat = (1 / kvb) / (1 + (vpk / kvb) * (vpk / kvb));
        if (e1 > 0) {
            const double pe = std::pow(e1, ex), dpe = ex * pe / e1;
            i[0] = 2 * pe / kg1 * at;
            J[0] = 2 * dpe / kg1 * at * sig;                                   // d/dvg1
            J[1] = v[1] > 0 ? 2 * pe / kg1 * dat : 0;                          // d/dvpk
            J[2] = v[2] > 1e-3 ? 2 * dpe / kg1 * at * (sp / kp - sig * vg1 / vg2) : 0;   // d/dvg2
        } else {
            i[0] = 0;
        }
        const double y = vg1 + vg2 / mu;
        if (y > 0) {
            i[1] = std::pow(y, ex) / kg2;
            const double dy = ex * i[1] / y;
            J[3] = dy;
            J[5] = v[2] > 1e-3 ? dy / mu : 0;
        } else {
            i[1] = 0;
        }
        gridCurrent(vg1, kgc, vs, i[2], J[6]);
    }
    bool limit(double* vnew, const double* vold) override
    {
        bool l = false;
        auto clamp = [&](double& vn, double vo, double maxStep) {
            if (std::abs(vn - vo) > maxStep) { vn = vo + std::copysign(maxStep, vn - vo); l = true; }
        };
        clamp(vnew[0], vold[0], 0.5);
        clamp(vnew[1], vold[1], 25.0);
        clamp(vnew[2], vold[2], 25.0);
        return l;
    }
};

// T4 electro-optical cell (LA-2A): an electroluminescent panel lighting a
// CdS photocell. Behavioural, fitted to UA's published LA-2A specs: very fast
// attack; ~40-80 ms to 50% release; 0.5-5 s for complete release depending on
// how much and how long it had been compressing ("memory").
//   port 0 = (control: EL panel voltage, current: none -- the panel's
//            electrical load is ordinary R/C in the netlist)
//   port 1 = (control: photocell voltage, current: G v -- the cell)
// State moves only in commit(): panel light L follows the EL drive (~1 ms);
// the cell's conductance has a fast part (releases with a ~60 ms half-life)
// and a slow part that builds under sustained light and releases over
// seconds, stretched by a memory state that tracks recent exposure.
struct T4Cell : Device {
    // EL panel: light ~ (|v| / vRef)^2 above a small threshold
    double vRef = 150, vThresh = 8, tauLight = 1e-3;
    // CdS: conductance at full light gFull, dark resistance rDark, photo response exponent
    double gFull = 1.0 / 400, rDark = 5e6, gamma = 0.8;
    // In dB, gain reduction goes roughly as log(1 + G R), so the fast part has to
    // carry most of the conductance for its release to take away about half the
    // dB: UA's "releases quickly to approximately half, the rest over seconds".
    double fastShare = 0.85;                          // share of the fast component
    double tauFastOn = 3e-3, tauFastOff = 0.03;
    double tauSlowOn = 0.3, tauSlowOff = 0.18;       // slow part; release stretched by memory
    double memStretch = 6;                           // slow release x (1 + memStretch * memory)
    double tauMemOn = 2.0, tauMemOff = 12.0;
    static std::vector<Port> ports(int elA, int elB, int cellA, int cellB)
    {
        return { { elA, elB, elA, elB }, { cellA, cellB, cellA, cellB } };
    }
    // External panel: the cell alone (one port); the EL panel voltage comes from
    // setPanelVoltage() -- e.g. a sidechain solved as a separate circuit.
    static std::vector<Port> cellPorts(int cellA, int cellB) { return { { cellA, cellB, cellA, cellB } }; }
    bool external = false;
    void setPanelVoltage(double v) { panelV = v; }

    int numPorts() const override { return external ? 1 : 2; }
    void setTimestep(double t) override { dt = t; }
    void reset() override { light = gFast = gSlow = memory = 0; }
    void eval(const double* v, double* i, double* J) override
    {
        const double g = conductance();
        if (external) {
            i[0] = g * v[0];
            J[0] = g;
            return;
        }
        i[0] = 0;
        i[1] = g * v[1];
        J[0] = J[1] = J[2] = 0;
        J[3] = g;
    }
    void commit(const double* v) override
    {
        const double x = std::max(0.0, std::abs(external ? panelV : v[0]) - vThresh) / vRef;
        light += (x * x - light) * (1 - std::exp(-dt / tauLight));
        const double drive = std::pow(std::max(light, 0.0), gamma);
        auto follow = [&](double& s, double target, double tOn, double tOff) {
            const double tau = target > s ? tOn : tOff;
            s += (target - s) * (1 - std::exp(-dt / tau));
        };
        follow(gFast, fastShare * drive, tauFastOn, tauFastOff);
        follow(gSlow, (1 - fastShare) * drive, tauSlowOn, tauSlowOff * (1 + memStretch * memory));
        follow(memory, std::min(1.0, drive * 4), tauMemOn, tauMemOff);
    }

    double conductance() const { return gFull * (gFast + gSlow) + 1 / rDark; }
    double resistance() const { return 1 / conductance(); }
    double panelLight() const { return light; }
    double cellMemory() const { return memory; }

private:
    double dt = 1.0 / 192000;
    double light = 0, gFast = 0, gSlow = 0, memory = 0, panelV = 0;
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

    // Langevin L(x) = coth x - 1/x and its slope, one tanh
    static void langevin(double x, double& l, double& dl)
    {
        if (std::abs(x) < 1e-4) { l = x / 3; dl = 1.0 / 3 - x * x / 15; return; }
        const double ct = 1 / std::tanh(x), ix = 1 / x;
        l = ct - ix;
        dl = ix * ix - (ct * ct - 1);   // 1/x^2 - csch^2 x
    }
    double dMdB(double b, double mag, double delta) const
    {
        const double h = b / mu0 - mag;
        const double x = std::clamp((h + alpha * mag) / a, -80.0, 80.0);
        double l, dl;
        langevin(x, l, dl);
        const double man = ms * l, dman = ms / a * dl;
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
