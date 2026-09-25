#include "PluginProcessor.h"
#include "PluginEditor.h"

// Knob ranges, curves and defaults come from core/circuit/Knobs.h (shared with the search).
juce::AudioProcessorValueTreeState::ParameterLayout CircuitDecimatorProcessor::createLayout()
{
    return cd::makeLayout(cd::kKnobs, cd::kNumKnobs);
}

CircuitDecimatorProcessor::CircuitDecimatorProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "state", createLayout())
{
}

void CircuitDecimatorProcessor::currentKnobs(double* out) const
{
    for (int i = 0; i < cd::kNumKnobs; ++i) out[i] = apvts.getRawParameterValue(cd::kKnobs[i].id)->load();
}

void CircuitDecimatorProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    double knobs[cd::kNumKnobs];
    currentKnobs(knobs);
    engine.prepare(sampleRate, samplesPerBlock, knobs);
    setLatencySamples(engine.latencySamples());
}

int CircuitDecimatorProcessor::getNumPrograms()
{
    return (int) cd::presets().size();
}

const juce::String CircuitDecimatorProcessor::getProgramName(int index)
{
    return juce::isPositiveAndBelow(index, getNumPrograms()) ? cd::presets()[(size_t) index].name : "";
}

void CircuitDecimatorProcessor::setCurrentProgram(int index)
{
    if (!juce::isPositiveAndBelow(index, getNumPrograms())) return;
    currentProgram = index;
    double v[cd::kNumKnobs];
    cd::presetKnobs(cd::presets()[(size_t) index], v);
    setKnobs(v, false);
}

void CircuitDecimatorProcessor::setKnobs(const double* values, bool includeLevels)
{
    for (int i = 0; i < cd::kNumKnobs; ++i) {
        if (!includeLevels && (i == cd::kVolume || i == cd::kInput || i == cd::kOutput)) continue;
        auto* prm = apvts.getParameter(cd::kKnobs[i].id);
        prm->setValueNotifyingHost(prm->convertTo0to1((float) values[i]));
    }
}

void CircuitDecimatorProcessor::reset()
{
    engine.reset();
}

bool CircuitDecimatorProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    const auto in = layouts.getMainInputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo()) return false;
    return in == juce::AudioChannelSet::mono() || in == out;
}

void CircuitDecimatorProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    double knobs[cd::kNumKnobs];
    currentKnobs(knobs);
    engine.process(buffer, getTotalNumInputChannels(), getTotalNumOutputChannels(), knobs);
}

juce::AudioProcessorEditor* CircuitDecimatorProcessor::createEditor()
{
    return new CircuitDecimatorEditor(*this);
}

void CircuitDecimatorProcessor::getStateInformation(juce::MemoryBlock& dest)
{
    if (auto xml = apvts.copyState().createXml()) copyXmlToBinary(*xml, dest);
}

void CircuitDecimatorProcessor::setStateInformation(const void* data, int size)
{
    if (auto xml = getXmlFromBinary(data, size))
        if (xml->hasTagName(apvts.state.getType())) apvts.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CircuitDecimatorProcessor();
}
