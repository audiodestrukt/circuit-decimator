#include "OptoEngine.h"

namespace opto {

void Engine::setTargets(const double* knobs, bool snap)
{
    gainDb.setTargetValue(knobs[kGain]);
    peak.setTargetValue(knobs[kPeak]);
    mix.setTargetValue((float) (knobs[kMix] / 100.0));
    if (snap) {
        gainDb.setCurrentAndTargetValue(knobs[kGain]);
        peak.setCurrentAndTargetValue(knobs[kPeak]);
        mix.setCurrentAndTargetValue(mix.getTargetValue());
    }
}

void Engine::applyPots()
{
    for (auto& c : ch) {
        c.setGain(gainWiper(gainDb.getCurrentValue()), c.prm);
        c.setPeak(peakWiper(peak.getCurrentValue()), c.prm);
        c.c.rebuildIfDirty();    // only while a knob moves
        c.sc.rebuildIfDirty();
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

    const double controlRate = osRate / controlInterval;
    gainDb.reset(controlRate, 0.05);
    peak.reset(controlRate, 0.05);
    mix.reset(sampleRate, 0.05);
    setTargets(knobs, true);

    cd::LA2AParams p;
    p.gain = gainWiper(knobs[kGain]);
    p.peak = peakWiper(knobs[kPeak]);
    for (auto& c : ch) {
        c = cd::LA2A {};
        c.build(p);
        c.c.maxIterations = 12;
        c.sc.maxIterations = 12;
        c.prepare(osRate, sidechainEvery);   // settled, cell dark
    }
    reset();
}

void Engine::reset()
{
    oversampling.reset();
    dry.reset();
    for (auto& f : dcBlock) f.reset();
    rmsState = 0;
}

void Engine::process(juce::AudioBuffer<float>& buffer, int numInputs, int numOutputs, const double* knobs)
{
    juce::ScopedNoDenormals noDenormals;
    setTargets(knobs, false);
    const int total = buffer.getNumSamples();
    const int numChannels = juce::jlimit(1, 2, numInputs);   // a mono input runs one unit
    const float inScale = (float) kInVolts;
    const double outScale = outputScale();

    for (int start = 0; start < total; start += maxBlock) {
        const int n = juce::jmin(maxBlock, total - start);
        work.setSize(2, n, false, false, true);
        dryWork.setSize(2, n, false, false, true);
        for (int c = 0; c < 2; ++c) {
            const int src = juce::jmin(c, juce::jmax(numInputs, 1) - 1);
            if (numInputs > 0) work.copyFrom(c, 0, buffer, src, start, n);
            else work.clear(c, 0, n);
            for (int i = 0; i < n; ++i) {
                dry.pushSample(c, work.getSample(c, i));
                dryWork.setSample(c, i, dry.popSample(c));
            }
        }

        juce::dsp::AudioBlock<float> block(work);
        auto up = oversampling.processSamplesUp(block);
        const int nUp = (int) up.getNumSamples();
        for (int i = 0; i < nUp; ++i) {
            if (i % controlInterval == 0) {
                if (gainDb.isSmoothing() || peak.isSmoothing()) {
                    gainDb.skip(controlInterval);
                    peak.skip(controlInterval);
                    applyPots();
                } else if (i == 0) {
                    applyPots();   // no-op unless a target jumped with smoothing off
                }
            }
            for (int c = 0; c < numChannels; ++c) {
                auto& u = ch[(size_t) c];
                float* x = up.getChannelPointer((size_t) c);
                u.process(x[i] * inScale);
                x[i] = (float) (u.output() * outScale);
            }
            if (numChannels == 1) up.getChannelPointer(1)[i] = up.getChannelPointer(0)[i];
        }
        oversampling.processSamplesDown(block);

        double sq = 0;
        for (int i = 0; i < n; ++i) {
            const float m = mix.getNextValue();
            for (int c = 0; c < 2; ++c) {
                float y = dcBlock[(size_t) c].processSample(work.getSample(c, i));
                if (!std::isfinite(y)) y = 0;
                if (c == 0) sq += (double) y * y;
                y = m * y + (1 - m) * dryWork.getSample(c, i);
                work.setSample(c, i, juce::jlimit(-2.0f, 2.0f, y));
            }
        }
        for (int c = 0; c < numOutputs; ++c) buffer.copyFrom(c, start, work, juce::jmin(c, 1), 0, n);

        // telemetry
        const float a = std::exp(-(float) n / (float) (0.3 * osRate / 2));   // ~300 ms VU-ish
        rmsState = a * rmsState + (1 - a) * (float) (sq / n);
        outputRms = std::sqrt(rmsState);
        size_t most = 0;
        double gr = 0;
        for (size_t c = 0; c < (size_t) numChannels; ++c) {
            const double g = ch[c].gainReductionDb();
            if (g > gr) { gr = g; most = c; }
        }
        gainReduction = (float) gr;
        panelLight = (float) ch[most].t4->panelLight();
        cellOhms = (float) ch[most].t4->resistance();
        failures = ch[0].c.failures + ch[0].sc.failures + ch[1].c.failures + ch[1].sc.failures;
    }
}

} // namespace opto
