#include "FuzzEngine.h"

namespace cd {

FuzzEngine::FuzzEngine()
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

    for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) kKnobs[i].def;  // until prepared
}

void FuzzEngine::setTargets(const double* knobs)
{
    target = knobsToCircuit(knobs);
    for (int i = 0; i < kNumKnobs; ++i) knobsInUse[(size_t) i] = (float) knobs[i];
    inputGain.setTargetValue(juce::Decibels::decibelsToGain((float) knobs[kInput]));
    outputGain.setTargetValue(juce::Decibels::decibelsToGain((float) knobs[kOutput]));
    for (auto& sm : smoothers) {
        const double v = target.*sm.field;
        sm.value.setTargetValue(sm.logDomain ? std::log(v) : v);
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
    inputGain.reset(sampleRate, 0.05);
    outputGain.reset(sampleRate, 0.05);
    setTargets(knobs);
    for (auto& sm : smoothers) sm.value.setCurrentAndTargetValue(sm.value.getTargetValue());
    inputGain.setCurrentAndTargetValue(inputGain.getTargetValue());
    outputGain.setCurrentAndTargetValue(outputGain.getTargetValue());

    circuit.setParams(target);
    circuit.prepare(osRate);   // solves the DC operating point
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

    if (resetRequested.exchange(false)) {
        for (auto& sm : smoothers) sm.value.setCurrentAndTargetValue(sm.value.getTargetValue());
        circuit.setParams(target);
        circuit.reset();
    }

    juce::dsp::AudioBlock<float> block(mono);
    auto up = oversampling.processSamplesUp(block);
    auto* x = up.getChannelPointer(0);
    const int nUp = (int) up.getNumSamples();
    auto p = circuit.params();
    long iterSum = 0;
    int iterMax = 0;
    std::array<double, kNodes> sum {}, lo, hi;
    lo.fill(1e9);
    hi.fill(-1e9);
    for (int i = 0; i < nUp; ++i) {
        if (i % controlInterval == 0) {
            bool changed = false;
            for (auto& sm : smoothers) {
                if (!sm.value.isSmoothing()) continue;
                sm.value.getNextValue();
                p.*sm.field = sm.current();
                changed = true;
            }
            if (changed) circuit.setParams(p);
        }
        x[i] = (float) circuit.process(x[i] * kPickupVolts);
        iterSum += circuit.lastIterations;
        iterMax = juce::jmax(iterMax, circuit.lastIterations);
        for (int k = 0; k < kNodes; ++k) {
            const double v = circuit.node((FuzzFace::Node) k);
            sum[(size_t) k] += v;
            lo[(size_t) k] = juce::jmin(lo[(size_t) k], v);
            hi[(size_t) k] = juce::jmax(hi[(size_t) k], v);
        }
    }
    if (nUp > 0) {
        for (size_t k = 0; k < (size_t) kNodes; ++k) {
            nodeMean[k] = (float) (sum[k] / nUp);
            nodeMin[k] = (float) lo[k];
            nodeMax[k] = (float) hi[k];
        }
    }
    newtonAverage = nUp > 0 ? (float) iterSum / (float) nUp : 0.0f;
    newtonMax = iterMax;
    solverFailures = circuit.failures;
    oversampling.processSamplesDown(block);

    for (int i = 0; i < n; ++i) {
        float y = dcBlock.processSample(m[i]) * (float) kOutputScale * outputGain.getNextValue();
        // last line of defence: a broken circuit must not blow up the host
        if (!std::isfinite(y)) y = 0;
        m[i] = juce::jlimit(-2.0f, 2.0f, y);
    }
    for (int c = 0; c < nOut; ++c) buffer.copyFrom(c, start, mono, 0, 0, n);
}

} // namespace cd
