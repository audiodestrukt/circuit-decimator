# Phys Fuzz: the focused product. Battery / Age / Temperature macro knobs over
# the shared circuit core, with the live schematic.
# The plugin code is this product's unique ID in hosts: fix it before release.
juce_add_plugin(PhysFuzz
    COMPANY_NAME "Audiodestrukt"
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Phfz
    PRODUCT_NAME "Phys Fuzz"
    BUNDLE_ID com.audiodestrukt.physfuzz
    FORMATS ${CD_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Distortion
    COPY_PLUGIN_AFTER_BUILD ${CD_COPY_PLUGINS})

target_sources(PhysFuzz PRIVATE PhysFuzzProcessor.cpp)

# Panel lettering (Michroma) and values (Barlow), both SIL Open Font License;
# the licences sit next to the fonts in assets/fonts.
juce_add_binary_data(PhysFuzzAssets SOURCES
    assets/fonts/Michroma-Regular.ttf
    assets/fonts/Barlow-SemiBold.ttf)
target_link_libraries(PhysFuzz
    PRIVATE cd_core PhysFuzzAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Dev tool: render the editor to PNG with audio playing (design review, store art).
if(CD_BUILD_TOOLS)
    juce_add_console_app(physfuzz_snapshot PRODUCT_NAME "physfuzz_snapshot")
    target_sources(physfuzz_snapshot PRIVATE tools/snapshot.cpp PhysFuzzProcessor.cpp)
    target_include_directories(physfuzz_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(physfuzz_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(physfuzz_snapshot
        PRIVATE cd_core PhysFuzzAssets juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
