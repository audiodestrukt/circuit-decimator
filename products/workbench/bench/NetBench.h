// NetBench -- Circuit Bench's path for the netlist circuits (core/circuit/
// circuits/Catalog.h): mono, 4x oversampled, component values applied at
// control rate from the UI, telemetry for the node meter and B-H loop.
//
// Circuits are built and prepared on the UI thread (select()), then handed to
// the audio thread at the next block; the replaced one is freed back on the
// UI thread (collect()), so the audio thread never allocates or frees.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include "circuit/circuits/Catalog.h"
#include "ui/BHLoopView.h"
#include "ui/NodeMeterView.h"

class NetBench {
public:
    static constexpr int kMaxControls = 96;
    std::array<std::atomic<double>, kMaxControls> values {};   // UI writes, audio applies

    cd::ui::BHTrace trace;
    cd::ui::ProbeTelemetry probes;
    std::atomic<float> newtonAverage { 0 };
    std::atomic<long> failures { 0 };
    std::atomic<float> gainReduction { NAN };   // dB, NaN for circuits without it

    ~NetBench()
    {
        delete pending.exchange(nullptr);
        delete retired.exchange(nullptr);
        delete current;
    }

    // audio thread (device start)
    void prepare(double sampleRate, int maxBlock)
    {
        fs = sampleRate;
        block = juce::jmax(1, maxBlock);
        oversampling.initProcessing((size_t) block);
        mono.setSize(1, block);
        dcBlock.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 10.0f);
        oversampling.reset();
        dcBlock.reset();
        if (current) current->prepare(rateFor(*current));
    }

    // the rate a circuit runs at: the host rate times its own oversampling
    double rateFor(const cd::catalog::BenchCircuit& c) const { return fs * (1 << c.oversamplingLog2()); }

    // what the UI shows for the selected circuit (UI thread only)
    std::vector<cd::catalog::Control> uiControls;
    std::vector<std::string> uiProbes;
    bool uiHasCore = false;
    bool uiHasGainReduction = false;
    bool uiHasSpeaker = false;
    // the circuit last selected (UI thread only; it's freed only on the UI thread, after a newer one replaces it)
    const cd::acoustic::Cab* uiCab() const { return uiCircuit ? uiCircuit->cabModel() : nullptr; }

    // UI thread: build circuit `index`, load its default values, hand it over
    void select(int index)
    {
        auto c = cd::catalog::make(index);
        if (!c) return;
        uiControls = c->controls();
        uiProbes.clear();
        for (auto& pr : c->probes()) uiProbes.emplace_back(pr.label);
        uiHasCore = c->core() != nullptr;
        uiHasGainReduction = std::isfinite(c->gainReductionDb());
        uiHasSpeaker = c->cabModel() != nullptr;
        uiCircuit = c.get();
        const auto& ctl = c->controls();
        std::vector<double> v(ctl.size());
        for (size_t k = 0; k < ctl.size() && k < (size_t) kMaxControls; ++k) {
            v[k] = ctl[k].knob.def;
            values[k] = ctl[k].knob.def;
        }
        c->prepare(rateFor(*c));   // allocate + DC point first; apply() rebuilds matrices
        c->apply(v.data());
        delete pending.exchange(c.release());
    }

    // UI thread: free a circuit the audio thread has let go of
    void collect() { delete retired.exchange(nullptr); }

    void requestWarmStart() { warmRequested = true; }

    // audio thread: mono in (summed), result copied to every output channel
    void process(juce::AudioBuffer<float>& buf, int n)
    {
        if (auto* p = pending.exchange(nullptr)) {
            auto* old = current;
            current = p;
            delete retired.exchange(old);   // a still-uncollected one is freed here, rarely
            oversampling.reset();
            dcBlock.reset();
        }
        if (!current) { buf.clear(); return; }
        if (warmRequested.exchange(false)) current->warmStart();

        const int nc = buf.getNumChannels();
        for (int start = 0; start < n; start += block) {
            const int len = juce::jmin(block, n - start);
            mono.setSize(1, len, false, false, true);
            for (int i = 0; i < len; ++i) {
                float s = 0;
                for (int c = 0; c < nc; ++c) s += buf.getSample(c, start + i);
                mono.setSample(0, i, s / (float) juce::jmax(1, nc));
            }
            juce::dsp::AudioBlock<float> ab(mono);
            // 4x for nonlinear circuits; a linear one (the speaker cab) at the host rate
            const bool os = current->oversamplingLog2() > 0;
            auto up = os ? oversampling.processSamplesUp(ab) : ab;
            float* x = up.getChannelPointer(0);
            const int nUp = (int) up.getNumSamples();
            const auto& prb = current->probes();
            const int np = (int) juce::jmin(prb.size(), (size_t) cd::ui::ProbeTelemetry::kMax);
            double sum[cd::ui::ProbeTelemetry::kMax] {}, lo[cd::ui::ProbeTelemetry::kMax], hi[cd::ui::ProbeTelemetry::kMax];
            for (int k = 0; k < np; ++k) { lo[k] = 1e9; hi[k] = -1e9; }
            long iters = 0;
            for (int i = 0; i < nUp; ++i) {
                if (i % 64 == 0) {
                    const size_t nv = juce::jmin(current->controls().size(), (size_t) kMaxControls);
                    for (size_t k = 0; k < nv; ++k) snapshot[k] = values[k].load(std::memory_order_relaxed);
                    current->apply(snapshot.data());
                }
                x[i] = (float) current->process(x[i]);
                iters += current->iterations();
                if (auto* core = current->core(); core && (i & 7) == 0)
                    trace.push((float) core->fluxDensity(), (float) core->field());
                if ((i & 3) == 0)
                    for (int k = 0; k < np; ++k) {
                        const double v = current->probeVoltage((size_t) k);
                        sum[k] += v;
                        lo[k] = std::min(lo[k], v);
                        hi[k] = std::max(hi[k], v);
                    }
            }
            const int probed = (nUp + 3) / 4;
            for (int k = 0; k < np; ++k) {
                probes.mean[(size_t) k] = (float) (sum[k] / probed);
                probes.swing[(size_t) k] = (float) (hi[k] - lo[k]);
            }
            newtonAverage = (float) iters / (float) juce::jmax(1, nUp);
            failures = current->failureCount();
            gainReduction = (float) current->gainReductionDb();
            if (os) oversampling.processSamplesDown(ab);
            for (int i = 0; i < len; ++i) {
                float y = dcBlock.processSample(mono.getSample(0, i));
                if (!std::isfinite(y)) y = 0;
                y = juce::jlimit(-2.0f, 2.0f, y);
                for (int c = 0; c < nc; ++c) buf.setSample(c, start + i, y);
            }
        }
    }

private:
    cd::catalog::BenchCircuit* current = nullptr;
    cd::catalog::BenchCircuit* uiCircuit = nullptr;
    std::atomic<cd::catalog::BenchCircuit*> pending { nullptr }, retired { nullptr };
    std::atomic<bool> warmRequested { false };
    std::array<double, kMaxControls> snapshot {};
    double fs = 48000;
    int block = 512;
    juce::dsp::Oversampling<float> oversampling { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false };
    juce::AudioBuffer<float> mono;
    juce::dsp::IIR::Filter<float> dcBlock;
};
