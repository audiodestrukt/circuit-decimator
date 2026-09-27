#include "ConeProcessor.h"
#include "ConeEditor.h"

namespace {

// speaker, box and mic; Auto Level, Level and Mix stay where the player left them
struct Preset {
    const char* name;
    double size, paper, weight, depth, dustcap, damping, volume, opening, micpos, micdist, micangle, mic;
};

constexpr Preset kPresets[] = {
    { "1x12 Closed, Cap", 12, 4.8, 441, 6.0, 0.7, 0.031, 50, 0, 0, 2.5, 0, 0 },
    { "1x12 Closed, Cap Edge", 12, 4.8, 441, 6.0, 0.7, 0.031, 50, 0, 5, 2.5, 0, 0 },
    { "1x12 Closed, Cone", 12, 4.8, 441, 6.0, 0.7, 0.031, 50, 0, 9, 2.5, 0, 0 },
    { "1x12 Open Back Combo", 12, 4.8, 441, 6.0, 0.7, 0.031, 40, 900, 3, 2.5, 0, 0 },
    { "Big Closed Box", 12, 4.8, 441, 6.0, 0.7, 0.031, 150, 0, 3, 2.5, 0, 0 },
    { "10\" Open Back", 10, 4.8, 441, 5.0, 0.5, 0.031, 30, 700, 2, 2.5, 0, 0 },
    { "Off-Axis Dark", 12, 4.8, 441, 6.0, 0.7, 0.031, 50, 0, 6, 4, 45, 0 },
    { "Room Distance (30 cm)", 12, 4.8, 441, 6.0, 0.7, 0.031, 50, 0, 3, 30, 0, 2 },
    { "Stiff Bright Cone", 12, 7.0, 380, 5.0, 0.4, 0.02, 50, 0, 2, 2.5, 0, 0 },
    { "Soft Heavy Cone", 12, 2.5, 700, 6.5, 1.5, 0.08, 50, 0, 2, 2.5, 0, 0 },
};

} // namespace

ConeProcessor::ConeProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "state", cd::makeLayout(cone::kKnobs, cone::kNumKnobs))
{
}

void ConeProcessor::knobs(double* out) const
{
    for (int i = 0; i < cone::kNumKnobs; ++i) out[i] = apvts.getRawParameterValue(cone::kKnobs[i].id)->load();
}

void ConeProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    double k[cone::kNumKnobs];
    knobs(k);
    engine.prepare(sampleRate, samplesPerBlock, k);
    setLatencySamples(engine.latencySamples());
}

bool ConeProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return in == juce::AudioChannelSet::mono() || in == out;
}

void ConeProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    double k[cone::kNumKnobs];
    knobs(k);
    engine.process(buffer, getTotalNumInputChannels(), getTotalNumOutputChannels(), k);
}

int ConeProcessor::getNumPrograms() { return (int) std::size(kPresets); }

const juce::String ConeProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? kPresets[index].name : "";
}

void ConeProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms())) return;
    currentProgram = index;
    const auto& p = kPresets[index];
    auto set = [&](const char* id, double v) {
        auto* prm = apvts.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1((float) v));
    };
    set("size", p.size);
    set("paper", p.paper);
    set("weight", p.weight);
    set("depth", p.depth);
    set("dustcap", p.dustcap);
    set("damping", p.damping);
    set("volume", p.volume);
    set("opening", p.opening);
    set("micpos", p.micpos);
    set("micdist", p.micdist);
    set("micangle", p.micangle);
    set("mictype", p.mic);
}

juce::AudioProcessorEditor* ConeProcessor::createEditor() { return new ConeEditor(*this); }

void ConeProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void ConeProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new ConeProcessor(); }
