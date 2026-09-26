#include "OptoProcessor.h"
#include "OptoEditor.h"

namespace {

struct Preset {
    const char* name;
    double peak, gain, mix;
};

constexpr Preset kPresets[] = {
    { "Vocal Leveler", 60, 0, 100 },
    { "Gentle Glue", 45, 0, 100 },
    { "Bass Evener", 55, 1, 100 },
    { "Squash", 85, 6, 100 },
    { "Parallel Squash", 90, 6, 45 },
};

} // namespace

OptoProcessor::OptoProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "state", cd::makeLayout(opto::kKnobs, opto::kNumKnobs))
{
}

void OptoProcessor::knobs(double* out) const
{
    for (int i = 0; i < opto::kNumKnobs; ++i) out[i] = apvts.getRawParameterValue(opto::kKnobs[i].id)->load();
}

void OptoProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    double k[opto::kNumKnobs];
    knobs(k);
    engine.prepare(sampleRate, samplesPerBlock, k);
    setLatencySamples(engine.latencySamples());
}

bool OptoProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return in == juce::AudioChannelSet::mono() || in == out;
}

void OptoProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    double k[opto::kNumKnobs];
    knobs(k);
    engine.process(buffer, getTotalNumInputChannels(), getTotalNumOutputChannels(), k);
}

int OptoProcessor::getNumPrograms() { return (int) std::size(kPresets); }

const juce::String OptoProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? kPresets[index].name : "";
}

void OptoProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms())) return;
    currentProgram = index;
    const auto& p = kPresets[index];
    auto set = [&](const char* id, double v) {
        auto* prm = apvts.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1((float) v));
    };
    set("peak", p.peak);
    set("gain", p.gain);
    set("mix", p.mix);
}

juce::AudioProcessorEditor* OptoProcessor::createEditor() { return new OptoEditor(*this); }

void OptoProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void OptoProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OptoProcessor(); }
