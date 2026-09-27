// CabModel.h -- the speaker cab simulator put together (docs/briefs/2026-09-26-speaker-cab.md):
//
//   per sample:  amp volts -> driver + box circuit (Speaker.h) -> coil velocity
//                -> convolution with the physics IR (Convolver.h) -> mic output
//   off the audio thread:  the IR, from Radiation.h (mic rings), Cone.h (breakup)
//                and Coupling.h (the cone's load on the motor), rebuilt when a
//                knob changes. Each stage is cached and rebuilt only when its own
//                inputs change: a mic move redoes the rings only, a cone change the
//                cone transfer, a driver or box change only the coupling.
//
// The IR is computed on a 48 kHz-ish grid (the base rate) and extended exactly
// to the oversampled rate: same bin spacing, zeros above the base band.
//
// Parameters travel as one flat array of doubles (CabParam) so the audio thread
// can hand them over through atomics without locking.
#pragma once

#include "Convolver.h"
#include "Coupling.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace cd::acoustic {

enum CabParam {
    // driver (Thiele-Small + lossy coil; the moving mass is built from its parts, see
    // driverOf) and box (volume, absorption, back opening, front size)
    kRe, kLe, kL2, kR2, kBl, kMotorMass, kCms, kRms, kVb, kQa, kOpenArea, kBaffle,
    // cone geometry
    kConeRadius, kCoilRadius, kDepth, kCurve, kDustCap, kCapHeight,
    // cone material, surround, dust cap
    kThickness, kDensity, kYoungs, kPoisson, kAniso, kRibs, kTaper, kLoss,
    kSurroundK, kSurroundKr, kSurroundR, kSurroundMass, kCapMass, kCapKr, kCapKrot,
    // microphone
    kMicOffset, kMicDistance, kMicAngle, kMicCapsule, kMicPattern, kMicModel, kMicFace,
    kNumCabParams
};
using CabParams = std::array<double, kNumCabParams>;

inline Cone coneOf(const CabParams& p);
inline ConeMaterial materialOf(const CabParams& p);

// the air load on both sides of the cone at low frequency (a baffled piston's mass)
inline double airMass(double radius) { return 2 * 8 * kRho * radius * radius * radius / 3; }

// Sd follows the cone's radius; Mms is built from its parts -- the voice coil and
// former (kMotorMass), the paper, surround and dust cap (integrated from the cone's
// geometry and material, exactly as the shell model does), and the air load -- so
// the cone's size and paper change the driver's mass, resonance and sensitivity.
inline DriverParams driverOf(const CabParams& p)
{
    DriverParams d;
    const double a = p[kConeRadius];
    d.re = p[kRe]; d.le = p[kLe]; d.l2 = p[kL2]; d.r2 = p[kR2]; d.bl = p[kBl];
    d.mms = p[kMotorMass] + airMass(a) + ConeModel::rigidMassOf(coneOf(p), materialOf(p));
    d.cms = p[kCms]; d.rms = p[kRms]; d.sd = M_PI * a * a;
    return d;
}
inline BoxParams boxOf(const CabParams& p)
{
    BoxParams b;
    b.vb = p[kVb]; b.qa = p[kQa]; b.open = p[kOpenArea]; b.baffle = p[kBaffle];
    return b;
}
inline Cone coneOf(const CabParams& p)
{
    Cone c;
    c.radius = p[kConeRadius]; c.coilRadius = p[kCoilRadius]; c.depth = p[kDepth]; c.curve = p[kCurve];
    c.dustCap = p[kDustCap]; c.capHeight = p[kCapHeight];
    return c;
}
inline ConeMaterial materialOf(const CabParams& p)
{
    ConeMaterial m;
    m.thickness = p[kThickness]; m.density = p[kDensity]; m.youngs = p[kYoungs]; m.poisson = p[kPoisson];
    m.anisotropy = p[kAniso]; m.ribs = p[kRibs]; m.taper = p[kTaper]; m.loss = p[kLoss];
    m.surroundK = p[kSurroundK]; m.surroundKr = p[kSurroundKr]; m.surroundR = p[kSurroundR];
    m.surroundMass = p[kSurroundMass]; m.dustCapMass = p[kCapMass]; m.dustCapKr = p[kCapKr]; m.dustCapKrot = p[kCapKrot];
    return m;
}
inline Mic micOf(const CabParams& p)
{
    Mic m;
    m.offset = p[kMicOffset]; m.distance = p[kMicDistance]; m.angle = p[kMicAngle];
    m.capsule = p[kMicCapsule]; m.pattern = p[kMicPattern];
    m.model = (int) std::lround(p[kMicModel]); m.face = p[kMicFace];
    return m;
}

// The Eminence Legend 1258 as calibrated in sim/speaker/calibrate.py (datasheet
// T/S set; coil inductance fitted to the published impedance; cone fitted to the
// published response), in a 50 l closed box, a cardioid 2 cm capsule at 2.5 cm.
inline CabParams legend1258()
{
    CabParams p {};
    p[kRe] = 7.44; p[kLe] = 0.563e-3; p[kL2] = 0.977e-3; p[kR2] = 6.84; p[kBl] = 10.9;
    p[kCms] = 8.96e-5; p[kRms] = 3.07;
    p[kVb] = 0.05; p[kQa] = 20; p[kOpenArea] = 0; p[kBaffle] = 0.45;
    p[kConeRadius] = std::sqrt(0.05067 / M_PI); p[kCoilRadius] = 0.019;
    p[kDepth] = 0.0599; p[kCurve] = 0.229; p[kDustCap] = 0.0525; p[kCapHeight] = 0.015;
    p[kThickness] = 0.303e-3; p[kDensity] = 441; p[kYoungs] = 4.785e9; p[kPoisson] = 0.3;
    p[kAniso] = 0.993; p[kRibs] = 2.22; p[kTaper] = 1.647; p[kLoss] = 0.0313;
    p[kSurroundK] = 0.4 / 8.96e-5; p[kSurroundKr] = 3.5e5; p[kSurroundR] = 0.778; p[kSurroundMass] = 2e-3;
    p[kCapMass] = 0.703e-3; p[kCapKr] = 5e5; p[kCapKrot] = 5.0;
    p[kMicOffset] = 0.0; p[kMicDistance] = 0.025; p[kMicAngle] = 0.0; p[kMicPattern] = 0.5;
    // a dynamic cardioid as measured (MicModels.h): its pattern, response, capsule, and 32 mm face
    p[kMicModel] = 1; p[kMicCapsule] = mics::kDynamicAperture; p[kMicFace] = mics::kDynamicFace;
    // the datasheet's Mms (32 g, air load included) less the cone, surround, dust cap and air
    p[kMotorMass] = 0.032 - airMass(p[kConeRadius]) - ConeModel::rigidMassOf(coneOf(p), materialOf(p));
    return p;
}

// What a view shows (Bench / product UI): the response at the mic and the cone's
// deflection shapes, filled by the worker after each rebuild.
struct CabDisplay {
    std::vector<double> freq, spl;               // response at the mic: dB SPL for 2.83 V at the amp
    std::vector<double> nodeR, nodeZ;            // the cone's shell-model nodes (m)
    std::vector<double> shapeFreq;               // frequencies the shapes are given at
    std::vector<std::vector<std::complex<double>>> shape;   // per freq: T at each node, then the dust cap
    CabParams params {};                         // what it was built from
    BoxParams box;                               // the box as built
    double mms = 0;                              // the driver's moving mass as built (kg)
};

// ---- the IR, stage-cached -----------------------------------------------------------
class CabIRBuilder {
public:
    static constexpr double kMaxFreq = 20000;
    static constexpr double kRAmp = 0.05;

    // fsOut: the rate the IR runs at; nOut: its length (power of 2). The base grid is
    // fsOut / k with k a power of 2 bringing it nearest 48 kHz.
    std::vector<double> build(const CabParams& p, double fsOut, size_t nOut)
    {
        size_t k = 1;
        while (fsOut / (double) (k * 2) >= 40000 && nOut / (k * 2) >= 256) k *= 2;
        const double fsB = fsOut / (double) k;
        const size_t nB = nOut / k;
        const double fmax = std::min(kMaxFreq, 0.45 * fsB);
        const Cone cone = coneOf(p);
        const ConeMaterial mat = materialOf(p);
        const bool gridChanged = std::abs(fsB - lastFs) > 0 || nB != lastN;
        const bool geomChanged = gridChanged || !same(p, lastP, kConeRadius, kCapHeight);
        const bool micChanged = geomChanged || !same(p, lastP, kMicOffset, kMicFace);
        const bool matChanged = geomChanged || !same(p, lastP, kThickness, kCapKrot);
        if (micChanged || !haveRings) {
            rings = acoustic::rings(cone, micOf(p), fsB, nB, fmax);
            haveRings = true;
        }
        if (matChanged || !haveCone) {
            model.build(cone, mat);
            ct = coneTransfer(model, rings.radius, fsB, nB, fmax);
            haveCone = true;
        }
        const auto cdrv = coupledDriver(driverOf(p), boxOf(p), kRAmp, ct, mat, cone.radius, fsB, nB);
        const auto vc = velocityCorrection(cdrv);
        RadiationIR r;
        r.spectrum = combine(rings, &ct, nullptr).spectrum;   // the front, per unit coil velocity
        applyBaffleEdges(r.spectrum, p, fsB, nB);              // the finite front baffle
        addRearWave(r.spectrum, p, cdrv, fsB, nB, fmax);        // an open back's rear wave
        for (size_t b = 0; b < r.spectrum.size(); ++b) r.spectrum[b] *= vc[b];
        lastSpectrum = r.spectrum;
        lastFsB = fsB;
        lastP = p;
        lastFs = fsB;
        lastN = nB;
        // extend to the output rate: same bin spacing, zeros above the base band
        std::vector<std::complex<double>> h(nOut, 0.0);
        for (size_t b = 0; b <= nB / 2; ++b) h[b] = r.spectrum[b];
        for (size_t b = 1; b <= nB / 2; ++b) h[nOut - b] = std::conj(r.spectrum[b]);
        if (nB / 2 < nOut / 2) h[nB / 2] *= 0.5, h[nOut - nB / 2] = std::conj(h[nB / 2]);
        fft(h, true);
        std::vector<double> ir(nOut);
        for (size_t i = 0; i < nOut; ++i) ir[i] = h[i].real();
        return ir;
    }

    // The front baffle is finite (kBaffle square, the speaker centred): its edges
    // diffract. Each point on the edge re-radiates the wave arriving there,
    // inverted and at half strength (the usual edge-source model of baffle-step
    // tools), with its own path and spreading to the mic:
    //   F(w) = 1 - 1/2 mean_i [ R_sm / (R_si + R_im) ] e^{-jk (R_si + R_im - R_sm)}
    // (source at the cone's centre, s; edge point i; mic m). At low frequency that
    // halves the pressure (the baffle step: half space -> whole space); at high
    // frequency the edge terms arrive spread in time and average out to a ripple;
    // close to the cone the edges are far off relative to it and it fades away.
    void applyBaffleEdges(std::vector<std::complex<double>>& spec, const CabParams& p, double fsB, size_t nB) const
    {
        const double b = p[kBaffle];
        if (b <= 0) return;
        const double mx = p[kMicOffset], mz = p[kMicDistance];
        const double sz = coneHeight(coneOf(p), 0.0) * 0.5;   // an acoustic centre between the dust cap and the rim
        const double Rsm = std::hypot(mx, mz - sz);
        constexpr int per = 32;                                 // points per side
        double wsum = 0;
        std::vector<std::pair<double, double>> edges;           // (extra path, weight)
        for (int side = 0; side < 4; ++side)
            for (int i = 0; i < per; ++i) {
                const double t = -b / 2 + b * (i + 0.5) / per;
                const double ex = side == 0 ? b / 2 : side == 1 ? -b / 2 : t;
                const double ey = side < 2 ? t : (side == 2 ? b / 2 : -b / 2);
                const double Rsi = std::sqrt(ex * ex + ey * ey + sz * sz);
                const double Rim = std::sqrt((ex - mx) * (ex - mx) + ey * ey + mz * mz);
                edges.push_back({ Rsi + Rim - Rsm, Rsm / (Rsi + Rim) });
                wsum += 1;
            }
        for (size_t k = 1; k < spec.size(); ++k) {
            const double kw = 2 * M_PI * fsB * (double) k / (double) nB / kC;
            std::complex<double> e = 0;
            for (const auto& [dl, wgt] : edges) e += wgt * std::polar(1.0, -kw * dl);
            spec[k] *= 1.0 - 0.5 * e / wsum;
        }
    }

    // An open back: the air the cone's rear pushes out of the opening reaches the mic
    // round the cabinet -- across the back to its edge, along the side, then from the
    // front edge to the mic -- inverted and delayed. The opening is a small monopole;
    // the mic picks it up through the same mic as the front (its pattern for the
    // direction it arrives from, the front edge, and its own response). Diffraction round the box: full strength while the box is small against
    // the wavelength, falling as 1/f above that (an approximation for the IR tuning
    // loop to refine). The finite front baffle's own edge (baffle step) isn't modelled.
    void addRearWave(std::vector<std::complex<double>>& spec, const CabParams& p, const CoupledDriver& cdrv,
                     double fsB, size_t nB, double fmax) const
    {
        const auto box = boxOf(p);
        if (box.open <= 0 || rings.radius.empty()) return;
        const double b = box.baffle, depth = box.vb / (b * b), ro = std::sqrt(box.open / M_PI);
        const double off = std::abs(p[kMicOffset]), dist = p[kMicDistance], ang = p[kMicAngle];
        const double legBack = std::max(0.0, b / 2 - ro), legSide = depth;
        const double dz = -dist, dr = b / 2 - off;                  // mic -> the front edge on its side
        const double r3 = std::max(1e-3, std::hypot(dz, dr));
        const double L = legBack + legSide + r3;
        const double ax = -std::cos(ang), ar = -std::sin(ang) * (p[kMicOffset] < 0 ? -1.0 : 1.0);
        const double cosPsi = (ax * dz + ar * dr) / r3;
        const double dring = 2 * rings.radius[0];                   // rings are uniform in radius
        const double t0 = std::min(0.42 * fsB, fmax), t1 = std::min(0.5 * fsB, std::max(0.42 * fsB + 1, fmax * 1.05));
        for (size_t k = 1; k < spec.size(); ++k) {
            const double f = fsB * (double) k / (double) nB, w = 2 * M_PI * f, kw = w / kC;
            if (f > t1) break;
            std::complex<double> ucone = 0;                         // the cone's rear volume velocity per coil velocity
            for (size_t j = 0; j < rings.radius.size(); ++j) ucone += ct.T[j][k] * (2 * M_PI * rings.radius[j] * dring);
            const auto uout = -ucone * cdrv.rearFraction[k];
            // through the same mic as the front: its pattern and its own (minimum-phase) response
            const double alpha = rings.alpha[k];
            const std::complex<double> g = (alpha + (1 - alpha) * cosPsi * (1.0 + 1.0 / std::complex<double>(0, kw * r3)))
                                           * rings.response[k];
            const double diffraction = 1 / (1 + kw * b / M_PI);
            const double taper = f <= t0 ? 1.0 : 0.5 * (1 + std::cos(M_PI * std::min(1.0, (f - t0) / std::max(1.0, t1 - t0))));
            spec[k] += std::complex<double>(0, w) * kRho / (4 * M_PI * L) * std::polar(1.0, -kw * L) * diffraction * g * uout * taper;
        }
    }

    // the display data for the last build()
    void display(const CabParams& p, CabDisplay& d) const
    {
        d.params = p;
        const size_t nb = lastSpectrum.size();
        if (nb < 2) return;
        const size_t nB = (nb - 1) * 2;
        const auto cdrv = coupledDriver(driverOf(p), boxOf(p), kRAmp, ct, materialOf(p), p[kConeRadius], lastFsB, nB);
        d.box = boxOf(p);
        d.mms = driverOf(p).mms;
        d.freq.clear();
        d.spl.clear();
        for (double f = 40; f <= std::min(16000.0, 0.45 * lastFsB); f *= 1.02) {
            const double bin = f * (double) nB / lastFsB;
            const size_t k = std::min(nb - 2, (size_t) bin);
            const double t = bin - (double) k;
            auto at = [&](size_t i) {   // circuit velocity per volt x the (coupled) mic transfer
                const auto uv = p[kBl] / (cdrv.ze[i] * cdrv.zm[i] + cdrv.bl2);
                return std::abs(uv * lastSpectrum[i]);
            };
            const double m = (1 - t) * at(std::max<size_t>(1, k)) + t * at(k + 1);
            d.freq.push_back(f);
            d.spl.push_back(20 * std::log10(std::max(1e-12, 2.83 * m / 20e-6)));
        }
        d.nodeR.clear();
        d.nodeZ.clear();
        for (int k = 0; k < model.numNodes(); ++k) {
            d.nodeR.push_back(model.nodeRadius(k));
            d.nodeZ.push_back(model.nodeZ(k));
        }
        d.shapeFreq.clear();
        d.shape.clear();
        std::vector<cplx> T;
        for (double f = 50; f <= 10000; f *= 1.06) {
            model.solve(2 * M_PI * f, T);
            T.push_back(model.dustCapResponse());
            d.shapeFreq.push_back(f);
            d.shape.push_back(T);
        }
    }

private:
    MicRings rings;
    ConeTransfer ct;
    ConeModel model;
    std::vector<std::complex<double>> lastSpectrum;
    double lastFsB = 48000;
    CabParams lastP {};
    double lastFs = 0;
    size_t lastN = 0;
    bool haveRings = false, haveCone = false;

    static bool same(const CabParams& a, const CabParams& b, int from, int to)
    {
        for (int i = from; i <= to; ++i)
            if (std::abs(a[(size_t) i] - b[(size_t) i]) > 0) return false;
        return true;
    }
};

// ---- the whole cab, realtime ---------------------------------------------------------
// Audio thread: prepare() once (allocates, builds the first IR synchronously), then
// set() at control rate and process() per sample. A worker thread rebuilds the IR
// when set() has changed something and hands it over lock-free.
class Cab {
public:
    Cab() : params(legend1258())
    {
        for (int i = 0; i < kNumCabParams; ++i) shared[(size_t) i] = params[(size_t) i];
    }
    ~Cab() { release(); }

    // fs: the rate process() runs at. irSeconds: IR length (rounded up to a power of 2 samples).
    void prepare(double sampleRate, double irSeconds = 0.045, size_t block = 64)
    {
        stop();
        fs = sampleRate;
        n = 1;
        while ((double) n < irSeconds * fs) n <<= 1;
        blockSize = block;
        conv.prepare(block, n / block);
        speaker = SpeakerBox {};
        speaker.build(driverOf(params), boxOf(params));
        speaker.c.prepare(fs);
        // first IR now, so audio starts with the right sound
        std::vector<IRSpectra*> held;
        conv.takeAll(held);
        for (auto* h : held) delete h;
        conv.setIR(IRSpectra::make(builder.build(params, fs, n), block).release());
        builtGen = requestedGen.load();
        publishDisplay(params);
        running = true;
        worker = std::thread([this] { run(); });
    }

    // control rate (audio thread): new parameter values
    void set(int index, double value)
    {
        auto& s = shared[(size_t) index];
        if (!(std::abs(s.load(std::memory_order_relaxed) - value) > 0)) return;
        s.store(value, std::memory_order_relaxed);
        params[(size_t) index] = value;
        if (index < kMicOffset) circuitDirty = true;   // the driver's mass follows the cone
        requestedGen.fetch_add(1, std::memory_order_release);
    }
    double get(int index) const { return params[(size_t) index]; }

    // ---- for views (UI thread): parameters as last set, and the display data
    CabParams paramsSnapshot() const
    {
        CabParams p;
        for (int i = 0; i < kNumCabParams; ++i) p[(size_t) i] = shared[(size_t) i].load(std::memory_order_relaxed);
        return p;
    }
    unsigned displayVersion() const { return dispVersion.load(std::memory_order_acquire); }
    CabDisplay display() const
    {
        std::lock_guard<std::mutex> l(dispMutex);
        return disp;
    }

    // the driver+box circuit tracks its knobs at once; the IR follows from the worker
    void applyCircuit()
    {
        if (!circuitDirty) return;
        circuitDirty = false;
        speaker.retune(driverOf(params), boxOf(params));
        auto& c = speaker.c;
        c.rebuildIfDirty();
    }

    // one sample: amp volts in, mic output (pascal-equivalent) out
    double process(double volts)
    {
        if (conv.canAccept())
            if (auto* p = pending.exchange(nullptr, std::memory_order_acq_rel)) conv.setIR(p);
        if (auto* r = conv.retired()) {
            IRSpectra* expected = nullptr;
            if (!trash.compare_exchange_strong(expected, r)) parked = r;   // worker hasn't emptied it yet
        } else if (parked) {
            IRSpectra* expected = nullptr;
            if (trash.compare_exchange_strong(expected, parked)) parked = nullptr;
        }
        speaker.c.setInput(speaker.input, volts);
        speaker.c.process();
        lastVelocity = speaker.c.out(speaker.velocity);
        return conv.process(lastVelocity);
    }

    double coilVelocity() const { return lastVelocity; }
    net::Circuit& circuit() { return speaker.c; }
    int latency() const { return (int) blockSize; }
    std::atomic<int> rebuilds { 0 };
    std::atomic<float> lastBuildMs { 0 };

private:
    CabParams params;                                   // audio thread's copy
    std::array<std::atomic<double>, kNumCabParams> shared;   // handed to the worker
    std::atomic<unsigned> requestedGen { 0 };
    unsigned builtGen = 0;
    bool circuitDirty = false;
    double fs = 192000, lastVelocity = 0;
    size_t n = 8192, blockSize = 64;
    SpeakerBox speaker;
    Convolver conv;
    CabIRBuilder builder;
    std::atomic<IRSpectra*> pending { nullptr }, trash { nullptr };
    IRSpectra* parked = nullptr;
    std::thread worker;
    std::atomic<bool> running { false };
    mutable std::mutex dispMutex;   // UI thread and worker only
    CabDisplay disp;
    std::atomic<unsigned> dispVersion { 0 };

    void publishDisplay(const CabParams& p)
    {
        CabDisplay d;
        builder.display(p, d);
        {
            std::lock_guard<std::mutex> l(dispMutex);
            disp = std::move(d);
        }
        dispVersion.fetch_add(1, std::memory_order_release);
    }

    void run()
    {
        while (running.load()) {
            delete trash.exchange(nullptr);
            const unsigned g = requestedGen.load(std::memory_order_acquire);
            if (g != builtGen) {
                CabParams p;
                for (int i = 0; i < kNumCabParams; ++i) p[(size_t) i] = shared[(size_t) i].load(std::memory_order_relaxed);
                const auto t0 = std::chrono::steady_clock::now();
                auto s = IRSpectra::make(builder.build(p, fs, n), blockSize);
                lastBuildMs = (float) std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                delete pending.exchange(s.release(), std::memory_order_acq_rel);   // an unclaimed older one goes
                builtGen = g;
                publishDisplay(p);
                ++rebuilds;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    void stop()
    {
        if (running.exchange(false) && worker.joinable()) worker.join();
        delete pending.exchange(nullptr);
        delete trash.exchange(nullptr);
        delete parked;
        parked = nullptr;
        while (auto* r = conv.retired()) delete r;
    }

public:
    // stop the worker and free every IR (not while process() may run)
    void release()
    {
        stop();
        std::vector<IRSpectra*> held;
        conv.takeAll(held);
        for (auto* h : held) delete h;
    }
};

} // namespace cd::acoustic
