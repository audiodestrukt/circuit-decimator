# Iron: a tube line stage into a single-ended, gapped output transformer,
# built from a netlist on the general engine (core/circuit/Circuit.h).
# The plugin code is this product's unique ID in hosts: fix it before release.
juce_add_plugin(Iron
    COMPANY_NAME "Audiodestrukt"
    BUNDLE_ID com.audiodestrukt.iron
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Iron
    PRODUCT_NAME "Iron"
    FORMATS ${CD_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Distortion
    COPY_PLUGIN_AFTER_BUILD ${CD_COPY_PLUGINS})

target_sources(Iron PRIVATE IronProcessor.cpp IronEngine.cpp)
target_include_directories(Iron PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# shares the family fonts and knob look with Phys Fuzz
target_link_libraries(Iron
    PRIVATE cd_core PhysFuzzAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(CD_BUILD_TOOLS)
    juce_add_console_app(iron_snapshot PRODUCT_NAME "iron_snapshot")
    target_sources(iron_snapshot PRIVATE tools/snapshot.cpp IronProcessor.cpp IronEngine.cpp)
    target_include_directories(iron_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(iron_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(iron_snapshot
        PRIVATE cd_core PhysFuzzAssets juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
