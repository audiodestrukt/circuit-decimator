// Build a JUCE parameter layout from a cd::Knob table (ranges, curves,
// defaults, units, display precision), so every product declares its knobs
// once, in plain C++, and hosts see them with the same curves the search uses.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "circuit/Knobs.h"

namespace cd {

inline juce::AudioProcessorValueTreeState::ParameterLayout makeLayout(const Knob* knobs, int count)
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> ps;
    for (int i = 0; i < count; ++i) {
        const auto& k = knobs[i];
        juce::NormalisableRange<float> r((float) k.min, (float) k.max);
        if (k.centre > k.min) r.setSkewForCentre((float) k.centre);
        const int decimals = k.decimals;
        auto attrs = juce::AudioParameterFloatAttributes().withLabel(k.unit).withStringFromValueFunction(
            [decimals](float v, int) { return juce::String(v, decimals); });
        ps.push_back(std::make_unique<juce::AudioParameterFloat>(juce::ParameterID { k.id, 1 }, k.name, r,
                                                                 (float) k.def, attrs));
    }
    return { ps.begin(), ps.end() };
}

} // namespace cd
