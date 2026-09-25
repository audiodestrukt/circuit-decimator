#include "PhysFuzzProcessor.h"
#include "PhysFuzzEditor.h"

PhysFuzzProcessor::PhysFuzzProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "state", cd::makeLayout(pf::kKnobs, pf::kNumKnobs))
{
}

// Phys Fuzz knobs -> the workbench's full component surface (Macros.h).
void PhysFuzzProcessor::workbenchKnobs(double* out) const
{
    double v[pf::kNumKnobs];
    for (int i = 0; i < pf::kNumKnobs; ++i) v[i] = apvts.getRawParameterValue(pf::kKnobs[i].id)->load();
    pf::toWorkbench(v, out);
}

void PhysFuzzProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    double knobs[cd::kNumKnobs];
    workbenchKnobs(knobs);
    engine.prepare(sampleRate, samplesPerBlock, knobs);
    setLatencySamples(engine.latencySamples());
}

bool PhysFuzzProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return in == juce::AudioChannelSet::mono() || in == out;
}

void PhysFuzzProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    double knobs[cd::kNumKnobs];
    workbenchKnobs(knobs);
    engine.process(buffer, getTotalNumInputChannels(), getTotalNumOutputChannels(), knobs);
}

int PhysFuzzProcessor::getNumPrograms()
{
    return (int) std::size(pf::kPresets);
}

const juce::String PhysFuzzProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? pf::kPresets[index].name : "";
}

void PhysFuzzProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms())) return;
    currentProgram = index;
    const auto& p = pf::kPresets[index];
    auto set = [&](const char* id, double v) {
        auto* prm = apvts.getParameter(id);
        prm->setValueNotifyingHost(prm->convertTo0to1((float) v));
    };
    set("battery", p.battery);
    set("age", p.age);
    set("temperature", p.temperature);
    set("fuzz", p.fuzz);
}

juce::AudioProcessorEditor* PhysFuzzProcessor::createEditor()
{
    return new PhysFuzzEditor(*this);
}

void PhysFuzzProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void PhysFuzzProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PhysFuzzProcessor();
}
