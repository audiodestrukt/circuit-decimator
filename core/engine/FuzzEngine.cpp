#include "FuzzEngine.h"

namespace cd {

FuzzEngine::FuzzEngine()
{
    {
        using F = FuzzFaceParams;
        auto s = [&](double F::* f, bool logDomain) { smoothers.push_back({ f, logDomain, {} }); };
        s(&F::fuzz, false);
        s(&F::vol, false);
        s(&F::vcc, false);
        s(&F::rbat, true);
        s(&F::cbulk, true);
        s(&F::rc2b, true);
        s(&F::bf1, true);
        s(&F::bf2, true);
        s(&F::rleak1, true);
        s(&F::rleak2, true);
        s(&F::rleakCin, true);
        s(&F::cfz, true);
        s(&F::tempC, false);
    }
    {
        using S = ShinEiParams;
        auto s = [&](double S::* f, bool logDomain) { seSmoothers.push_back({ f, logDomain, {} }); };
        s(&S::fuzz, false);
        s(&S::vol, false);
        s(&S::vcc, false);
        s(&S::rbat, true);
        s(&S::cbulk, true);
        s(&S::rb2, true);
        s(&S::bf1, true);
        s(&S::bf2, true);
        s(&S::rleak1, true);
        s(&S::rleak2, true);
        s(&S::rleakCin, true);
        s(&S::tempC, false);
    }

    for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) kKnobs[i].def;  // until prepared
}

void FuzzEngine::setTargets(const double* knobs)
{
    target = knobsToCircuit(knobs);
    seTarget = knobsToShinEi(knobs);
    for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) knobs[i];
    inputGain.setTargetValue(juce::Decibels::decibelsToGain((float) knobs[kInput]));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain((float) knobs[kOutput]));
    for (auto& sm : smoothers) sm.target(target);
    for (auto& sm : seSmoothers) sm.target(seTarget);
}

void FuzzEngine::snapSmoothers()
{
    for (auto& sm : smoothers) sm.value.setCurrentAndTargetValue(sm.value.getTargetValue());
    for (auto& sm : seSmoothers) sm.value.setCurrentAndTargetValue(sm.value.getTargetValue());
}

// Put circuit `model` at its target values and re-solve its DC operating point.
void FuzzEngine::restart(int model)
{
    if (model == (int) Model::FuzzFace) {
        circuit.setParams(target);
        circuit.reset();
    } else {
        shinei.apply(seTarget);
        shinei.c.warmStart();
    }
}

void FuzzEngine::prepare(double sampleRate, int maxBlockSize, const double* knobs)
{
    const double osRate = sampleRate * (1 << osFactorLog2);
    maxBlock = juce::jmax(1, maxBlockSize);
    oversampling.initProcessing((size_t) maxBlock);
    mono.setSize(1, maxBlock);
    dcBlock.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass(sampleRate, 15.0f);

    for (auto& sm : smoothers) sm.value.reset(osRate / controlInterval, 0.05);  // ramp in control steps
    for (auto& sm : seSmoothers) sm.value.reset(osRate / controlInterval, 0.05);
    inputGain.reset(sampleRate, 0.05);
    outputGain.reset(sampleRate, 0.05);
    setTargets(knobs);
    snapSmoothers();
    inputGain.setCurrentAndTargetValue(inputGain.getTargetValue());
    outputGain.setCurrentAndTargetValue(outputGain.getTargetValue());

    circuit.setParams(target);
    circuit.prepare(osRate);   // solves the DC operating point

    shinei = ShinEi {};        // (re)build the netlist at this rate
    shinei.build(seTarget);
    shinei.c.maxIterations = 8;
    shinei.c.prepare(osRate);

    activeModel = requestedModel.load();
    modelInUse = activeModel;
    reset();
}

void FuzzEngine::reset()
{
    oversampling.reset();
    dcBlock.reset();
}

int FuzzEngine::latencySamples() const
{
    return (int) std::round(oversampling.getLatencyInSamples());
}

void FuzzEngine::process(juce::AudioBuffer<float>& buffer, int numInputs, int numOutputs, const double* knobs)
{
    juce::ScopedNoDenormals noDenormals;
    setTargets(knobs);
    // hosts may exceed the announced block size; the oversampler can't
    for (int start = 0; start < buffer.getNumSamples(); start += maxBlock)
        processChunk(buffer, start, juce::jmin(maxBlock, buffer.getNumSamples() - start), numInputs, numOutputs);
}

void FuzzEngine::processChunk(juce::AudioBuffer<float>& buffer, int start, int n, int nIn, int nOut)
{
    // the circuit is mono: sum inputs, then feed the pickup
    mono.setSize(1, n, false, false, true);  // n <= maxBlock: no allocation
    auto* m = mono.getWritePointer(0);
    for (int i = 0; i < n; ++i) {
        float s = 0;
        for (int c = 0; c < nIn; ++c) s += buffer.getReadPointer(c)[start + i];
        m[i] = s / (float) juce::jmax(1, nIn) * inputGain.getNextValue();
    }

    const int wanted = requestedModel.load();
    if (resetRequested.exchange(false) || wanted != activeModel) {
        // power cycle, or a circuit change: land on the target values, settled
        snapSmoothers();
        activeModel = wanted;
        modelInUse = activeModel;
        restart(activeModel);
    }
    const bool ff = activeModel == (int) Model::FuzzFace;

    juce::dsp::AudioBlock<float> block(mono);
    auto up = oversampling.processSamplesUp(block);
    auto* x = up.getChannelPointer(0);
    const int nUp = (int) up.getNumSamples();
    auto p = circuit.params();
    auto sp = seTarget;
    for (auto& sm : seSmoothers) sp.*sm.field = sm.current();
    const int nn = ff ? (int) FuzzFace::N : (int) ShinEi::N;
    // each circuit's output scale is applied before the downsampler, so both
    // present the same level to its filters and a switch doesn't ring them
    const double scale = ff ? kOutputScale : kOutputScaleShinEi;
    long iterSum = 0;
    int iterMax = 0;
    std::array<double, kNodes> sum {}, lo, hi;
    lo.fill(1e9);
    hi.fill(-1e9);
    for (int i = 0; i < nUp; ++i) {
        if (i % controlInterval == 0) {
            bool changed = false;
            if (ff) {
                for (auto& sm : smoothers) {
                    if (!sm.value.isSmoothing()) continue;
                    sm.value.getNextValue();
                    p.*sm.field = sm.current();
                    changed = true;
                }
                if (changed) circuit.setParams(p);
            } else {
                for (auto& sm : seSmoothers) {
                    if (!sm.value.isSmoothing()) continue;
                    sm.value.getNextValue();
                    sp.*sm.field = sm.current();
                    changed = true;
                }
                if (changed) shinei.apply(sp);
            }
        }
        if (ff) {
            x[i] = (float) (circuit.process(x[i] * kPickupVolts) * scale);
            iterSum += circuit.lastIterations;
            iterMax = juce::jmax(iterMax, circuit.lastIterations);
        } else {
            x[i] = (float) (shinei.process(x[i] * kPickupVolts) * scale);
            iterSum += shinei.c.lastIterations;
            iterMax = juce::jmax(iterMax, shinei.c.lastIterations);
        }
        if ((i & 3) != 0) continue;   // telemetry at 1/4 of the oversampled rate is plenty
        for (int k = 0; k < nn; ++k) {
            const double v = ff ? circuit.node((FuzzFace::Node) k) : shinei.node((ShinEi::Node) k);
            sum[(size_t) k] += v;
            lo[(size_t) k] = juce::jmin(lo[(size_t) k], v);
            hi[(size_t) k] = juce::jmax(hi[(size_t) k], v);
        }
    }
    if (nUp > 0) {
        for (size_t k = 0; k < (size_t) nn; ++k) {
            nodeMean[k] = (float) (sum[k] / ((nUp + 3) / 4));
            nodeMin[k] = (float) lo[k];
            nodeMax[k] = (float) hi[k];
        }
    }
    newtonAverage = nUp > 0 ? (float) iterSum / (float) nUp : 0.0f;
    newtonMax = iterMax;
    solverFailures = ff ? circuit.failures : shinei.c.failures;
    oversampling.processSamplesDown(block);

    for (int i = 0; i < n; ++i) {
        float y = dcBlock.processSample(m[i]) * outputGain.getNextValue();
        // last line of defence: a broken circuit must not blow up the host
        if (!std::isfinite(y)) y = 0;
        m[i] = juce::jlimit(-2.0f, 2.0f, y);
    }
    for (int c = 0; c < nOut; ++c) buffer.copyFrom(c, start, mono, 0, 0, n);
}

} // namespace cd
