# Phys Fuzz: the focused product. Battery / Age / Temperature macro knobs over
# the shared circuit core, with the live schematic.
# The plugin code is this product's unique ID in hosts: fix it before release.
juce_add_plugin(PhysFuzz
    COMPANY_NAME "Audiodestrukt"
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Phfz
    PRODUCT_NAME "Phys Fuzz"
    FORMATS VST3 Standalone
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Distortion
    COPY_PLUGIN_AFTER_BUILD TRUE)

target_sources(PhysFuzz PRIVATE PhysFuzzProcessor.cpp)
target_link_libraries(PhysFuzz
    PRIVATE cd_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
