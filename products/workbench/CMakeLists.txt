# Workbench: every component knob, the live schematic, sound-search presets.
# The manufacturer + plugin codes form the plugin's unique ID in hosts; they
# must not change once anyone has it in a saved session.
juce_add_plugin(CircuitDecimator
    COMPANY_NAME "Audiodestrukt"
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Cdcm
    PRODUCT_NAME "Circuit Decimator"
    BUNDLE_ID com.audiodestrukt.circuitdecimator
    FORMATS ${CD_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Distortion
    COPY_PLUGIN_AFTER_BUILD ${CD_COPY_PLUGINS})

target_sources(CircuitDecimator PRIVATE PluginProcessor.cpp)
target_link_libraries(CircuitDecimator
    PRIVATE cd_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Circuit Bench: loops a WAV (or live input) through the workbench processor,
# with presets, search results, bypass, scope, schematic and solver stats.
juce_add_gui_app(CircuitBench PRODUCT_NAME "Circuit Bench" BUNDLE_ID com.audiodestrukt.circuitbench)
target_sources(CircuitBench PRIVATE bench/Main.cpp PluginProcessor.cpp)
target_include_directories(CircuitBench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(CircuitBench PRIVATE
    JUCE_APPLICATION_NAME_STRING="Circuit Bench"
    JUCE_APPLICATION_VERSION_STRING="0.1.0"
    "CD_DEFAULT_INPUT=\"${CMAKE_SOURCE_DIR}/sim/renders/input.wav\""
    "CD_SEARCH_DIR=\"${CMAKE_SOURCE_DIR}/search/runs\"")
target_link_libraries(CircuitBench
    PRIVATE cd_core
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(CD_BUILD_TOOLS)
    # Render the speaker cross-section view for given cab parameters (design review).
    juce_add_console_app(speaker_snapshot PRODUCT_NAME "speaker_snapshot")
    target_sources(speaker_snapshot PRIVATE tools/speaker_snapshot.cpp)
    target_compile_definitions(speaker_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(speaker_snapshot
        PRIVATE cd_core
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
