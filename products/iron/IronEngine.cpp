#include "IronEngine.h"

namespace iron {

void Engine::setTargets(const double* knobs, bool snap)
{
    const auto cs = coreFor(knobs);
    lg.setTargetValue(cs.lg);
    ac.setTargetValue(cs.ac);
    kpin.setTargetValue(cs.k);
    crev.setTargetValue(cs.c);
    gIn.setTargetValue((float) inputGain(knobs));
    gOut.setTargetValue((float) outputGain(knobs));
    mix.setTargetValue((float) (knobs[kMix] / 100.0));
    if (snap) {
        lg.setCurrentAndTargetValue(cs.lg);
        ac.setCurrentAndTargetValue(cs.ac);
        kpin.setCurrentAndTargetValue(cs.k);
        crev.setCurrentAndTargetValue(cs.c);
        gIn.setCurrentAndTargetValue(gIn.getTargetValue());
        gOut.setCurrentAndTargetValue(gOut.getTargetValue());
        mix.setCurrentAndTargetValue(mix.getTargetValue());
    }
    for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) knobs[i];
}

void Engine::applyCore()
{
    for (auto& c : ch) {
        c.stage.core->lg = lg.getCurrentValue();
        c.stage.core->ac = ac.getCurrentValue();
        c.stage.core->k = kpin.getCurrentValue();
        c.stage.core->c = crev.getCurrentValue();
    }
}

void Engine::prepare(double sampleRate, int maxBlockSize, const double* knobs)
{
    maxBlock = juce::jmax(1, maxBlockSize);
    osRate = sampleRate * (1 << osLog2);
    oversampling.initProcessing((size_t) maxBlock);
    latency = (int) std::round(oversampling.getLatencyInSamples());
    work.setSize(2, maxBlock);
    dryWork.setSize(2, maxBlock);
    dry.prepare({ sampleRate, (juce::uint32) maxBlock, 2 });
    dry.setMaximumDelayInSamples(juce::jmax(latency + 1, 8));
    dry.setDelay((float) latency);
    for (auto& f : dcBlock) f.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 10.0f);

    // control-rate smoothing (50 ms)
    const double controlRate = osRate / controlInterval;
    lg.reset(controlRate, 0.05);
    ac.reset(controlRate, 0.05);
    kpin.reset(controlRate, 0.05);
    crev.reset(controlRate, 0.05);
    gIn.reset(sampleRate, 0.05);
    gOut.reset(sampleRate, 0.05);
    mix.reset(sampleRate, 0.05);
    setTargets(knobs, true);

    const auto cs = coreFor(knobs);
    cd::SEOutputParams p;
    p.lg = cs.lg;
    p.ac = cs.ac;
    p.k = cs.k;
    p.c = cs.c;
    bplus = p.bplus;
    for (auto& c : ch) {
        c.stage = cd::SEOutput {};
        c.stage.build(p);
        c.stage.c.maxIterations = 12;
        c.stage.c.prepare(osRate);   // warm start: settled, core on its initial curve
        const char* names[kProbes] = { "grid", "cath", "plate", "bp", "m", "out" };
        for (int k = 0; k < kProbes; ++k) c.probeNodes[k] = c.stage.c.node(names[k]);
    }
    coldSamples = -1;
    reset();
}

void Engine::reset()
{
    oversampling.reset();
    dry.reset();
    for (auto& f : dcBlock) f.reset();
}

void Engine::process(juce::AudioBuffer<float>& buffer, int numInputs, int numOutputs, const double* knobs)
{
    juce::ScopedNoDenormals noDenormals;
    setTargets(knobs, false);
    const int total = buffer.getNumSamples();

    if (coldRequested.exchange(false)) {
        for (auto& c : ch) {
            c.stage.c.zeroState();
            c.stage.c.setInput(c.stage.supply, 0.0);
        }
        coldSamples = 0;
    }

    for (int start = 0; start < total; start += maxBlock) {
        const int n = juce::jmin(maxBlock, total - start);
        numChannels = juce::jlimit(1, 2, juce::jmax(numInputs, 1));

        // input (mono input feeds both circuits), drive
        work.setSize(2, n, false, false, true);
        for (int c = 0; c < 2; ++c) {
            const int src = juce::jmin(c, juce::jmax(numInputs, 1) - 1);
            if (numInputs > 0) work.copyFrom(c, 0, buffer, src, start, n);
            else work.clear(c, 0, n);
        }
        // dry path for Mix, delayed to line up with the oversampled wet path
        dryWork.setSize(2, n, false, false, true);   // n <= maxBlock: no allocation
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i) {
                dry.pushSample(c, work.getSample(c, i));
                dryWork.setSample(c, i, dry.popSample(c));
            }

        for (int i = 0; i < n; ++i) {
            const float g = gIn.getNextValue();
            for (int c = 0; c < 2; ++c) work.setSample(c, i, work.getSample(c, i) * g);
        }

        juce::dsp::AudioBlock<float> block(work);
        auto up = oversampling.processSamplesUp(block);
        const int nUp = (int) up.getNumSamples();

        double sum[kProbes] {}, lo[kProbes], hi[kProbes], fluxSum = 0;
        for (int k = 0; k < kProbes; ++k) { lo[k] = 1e9; hi[k] = -1e9; }
        long iters = 0;
        for (int i = 0; i < nUp; ++i) {
            if (i % controlInterval == 0) {
                lg.getNextValue(); ac.getNextValue(); kpin.getNextValue(); crev.getNextValue();
                applyCore();
                if (coldSamples >= 0) {
                    const double t = (double) coldSamples / osRate;
                    const double v = bplus * juce::jmin(1.0, t / coldRampSeconds);
                    for (auto& c : ch) c.stage.c.setInput(c.stage.supply, v);
                    if (t >= coldRampSeconds) coldSamples = -1;
                }
            }
            if (coldSamples >= 0) ++coldSamples;
            for (int c = 0; c < numChannels; ++c) {
                auto& st = ch[(size_t) c].stage;
                float* x = up.getChannelPointer((size_t) c);
                st.c.setInput(st.input, x[i]);
                st.c.process();
                x[i] = (float) st.c.out(st.out);
                if (c == 0) iters += st.c.lastIterations;
            }
            if (numChannels == 1) up.getChannelPointer(1)[i] = up.getChannelPointer(0)[i];

            // telemetry from channel 0
            auto& s0 = ch[0].stage;
            fluxSum += s0.core->fluxDensity();
            if (++traceDecim >= 8) {
                traceDecim = 0;
                trace.push((float) s0.core->fluxDensity(), (float) s0.core->field());
            }
            if ((i & 3) == 0) {
                for (int k = 0; k < kProbes; ++k) {
                    const double v = s0.c.x(ch[0].probeNodes[k]);
                    sum[k] += v;
                    lo[k] = std::min(lo[k], v);
                    hi[k] = std::max(hi[k], v);
                }
            }
        }
        const int probed = (nUp + 3) / 4;
        for (int k = 0; k < kProbes; ++k) {
            probeMean[(size_t) k] = (float) (sum[k] / probed);
            probeSwing[(size_t) k] = (float) (hi[k] - lo[k]);
        }
        fluxMean = (float) (fluxSum / nUp);
        supplyVolts = (float) ch[0].stage.c.getInput(ch[0].stage.supply);
        newtonAverage = (float) iters / (float) nUp;
        failures = ch[0].stage.c.failures + ch[1].stage.c.failures;

        oversampling.processSamplesDown(block);

        for (int i = 0; i < n; ++i) {
            const float go = gOut.getNextValue(), m = mix.getNextValue();
            for (int c = 0; c < 2; ++c) {
                float y = dcBlock[(size_t) c].processSample(work.getSample(c, i)) * go;
                y = m * y + (1 - m) * dryWork.getSample(c, i);
                if (!std::isfinite(y)) y = 0;
                work.setSample(c, i, juce::jlimit(-2.0f, 2.0f, y));
            }
        }
        for (int c = 0; c < numOutputs; ++c) buffer.copyFrom(c, start, work, juce::jmin(c, 1), 0, n);
    }
}

} // namespace iron
