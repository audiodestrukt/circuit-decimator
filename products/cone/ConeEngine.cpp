#include "ConeEngine.h"

namespace cone {

void Engine::setKnobs(const double* knobs)
{
    for (auto& c : cabs)
        if (c) {
            applyKnobs(knobs, [&](int i, double v) { c->set(i, v); });
            c->applyCircuit();
        }
}

// Output scale: the cab gives pressure at the mic; scale so the calibrated default
// plays at about unity across 200 Hz - 4 kHz. With Auto Level, follow the current
// setting's own broadband level instead, so the mic and box change the tone, not
// the volume.
float Engine::targetGain(const double* knobs) const
{
    const double band = knobs[kAutoLevel] > 0.5 && cabs[0] ? (double) cabs[0]->bandLevelDb() : refBandDb;
    // |H| (pressure per volt) at the band = 20e-6 / 2.83 * 10^(band / 20)
    const double h = 20e-6 / 2.83 * std::pow(10.0, band / 20);
    return (float) (1.0 / (ampVolts(knobs) * h) * std::pow(10.0, knobs[kLevel] / 20));
}

void Engine::prepare(double sampleRate, int maxBlockSize, const double* knobs)
{
    release();
    fs = sampleRate;
    maxBlock = juce::jmax(1, maxBlockSize);
    for (auto& c : cabs) {
        c = std::make_unique<cd::acoustic::Cab>();
        applyKnobs(knobs, [&](int i, double v) { c->set(i, v); });
        c->prepare(fs);   // builds the first IR here, then a worker takes over
    }
    latency = cabs[0]->latency();
    // the reference level: the calibrated default speaker, box and mic
    {
        cd::acoustic::CabIRBuilder b;
        const auto p = cd::acoustic::legend1258();
        b.build(p, 48000, 4096);
        cd::acoustic::CabDisplay d;
        b.display(p, d);
        refBandDb = cd::acoustic::bandLevelOf(d);
    }
    dry.prepare({ sampleRate, (juce::uint32) maxBlock, 2 });
    dry.setMaximumDelayInSamples(juce::jmax(latency + 1, 8));
    dry.setDelay((float) latency);
    dryWork.setSize(2, maxBlock);
    gain.reset(sampleRate, 0.1);
    mix.reset(sampleRate, 0.05);
    gain.setCurrentAndTargetValue(targetGain(knobs));
    mix.setCurrentAndTargetValue((float) (knobs[kMix] / 100));
}

void Engine::reset()
{
    dry.reset();
}

void Engine::release()
{
    for (auto& c : cabs) {
        if (c) c->release();
        c.reset();
    }
}

void Engine::process(juce::AudioBuffer<float>& buffer, int numInputs, int numOutputs, const double* knobs)
{
    juce::ScopedNoDenormals noDenormals;
    if (!cabs[0]) { buffer.clear(); return; }
    setKnobs(knobs);
    gain.setTargetValue(targetGain(knobs));
    mix.setTargetValue((float) (knobs[kMix] / 100));
    const double volts = ampVolts(knobs);
    const int total = buffer.getNumSamples();
    const int channels = juce::jlimit(1, 2, numInputs);   // a mono input runs one cab
    for (int start = 0; start < total; start += maxBlock) {
        const int n = juce::jmin(maxBlock, total - start);
        dryWork.setSize(2, n, false, false, true);
        for (int c = 0; c < 2; ++c) {
            const int src = juce::jmin(c, juce::jmax(numInputs, 1) - 1);
            for (int i = 0; i < n; ++i) {
                dry.pushSample(c, numInputs > 0 ? buffer.getSample(src, start + i) : 0.0f);
                dryWork.setSample(c, i, dry.popSample(c));
            }
        }
        for (int i = 0; i < n; ++i) {
            const float g = gain.getNextValue(), m = mix.getNextValue();
            float y[2] {};
            for (int c = 0; c < channels; ++c) {
                const float x = numInputs > 0 ? buffer.getSample(c, start + i) : 0.0f;
                y[c] = (float) cabs[(size_t) c]->process(volts * (double) x) * g;
                if (!std::isfinite(y[c])) y[c] = 0;
            }
            if (channels == 1) y[1] = y[0];
            for (int c = 0; c < numOutputs; ++c) {
                const int k = juce::jmin(c, 1);
                buffer.setSample(c, start + i, juce::jlimit(-2.0f, 2.0f, m * y[k] + (1 - m) * dryWork.getSample(k, i)));
            }
        }
    }
}

} // namespace cone
