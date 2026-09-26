#include "IronProcessor.h"
#include "IronEditor.h"

namespace {

struct Preset {
    const char* name;
    double drive, gap, core, steel;
};

// Output and Mix stay where the player left them.
constexpr Preset kPresets[] = {
    { "Warm", 0, 0.1, 100, 30 },
    { "Clean Iron", -6, 0.2, 150, 10 },
    { "Fat Bottom", 6, 0.05, 120, 30 },
    { "Cheap Tranny", 6, 0.1, 60, 90 },
    { "Blown", 18, 0.03, 50, 100 },
};

} // namespace

IronProcessor::IronProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "state", cd::makeLayout(iron::kKnobs, iron::kNumKnobs))
{
}

void IronProcessor::knobs(double* out) const
{
    for (int i = 0; i < iron::kNumKnobs; ++i) out[i] = apvts.getRawParameterValue(iron::kKnobs[i].id)->load();
}

void IronProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    double k[iron::kNumKnobs];
    knobs(k);
    engine.prepare(sampleRate, samplesPerBlock, k);
    setLatencySamples(engine.latencySamples());
}

bool IronProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return in == juce::AudioChannelSet::mono() || in == out;
}

void IronProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    double k[iron::kNumKnobs];
    knobs(k);
    engine.process(buffer, getTotalNumInputChannels(), getTotalNumOutputChannels(), k);
}

int IronProcessor::getNumPrograms() { return (int) std::size(kPresets); }

const juce::String IronProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? kPresets[index].name : "";
}

void IronProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms())) return;
    currentProgram = index;
    const auto& p = kPresets[index];
    auto set = [&](const char* id, double v) {
        auto* prm = apvts.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1((float) v));
    };
    set("drive", p.drive);
    set("gap", p.gap);
    set("core", p.core);
    set("steel", p.steel);
}

juce::AudioProcessorEditor* IronProcessor::createEditor() { return new IronEditor(*this); }

void IronProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void IronProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new IronProcessor(); }
