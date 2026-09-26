# Opto: an LA-2A-style optical leveling amplifier (T4 cell, tubes, transformers),
# built from a netlist on the general engine (core/circuit/Circuit.h).
# The plugin code is this product's unique ID in hosts: fix it before release.
juce_add_plugin(Opto
    COMPANY_NAME "Audiodestrukt"
    BUNDLE_ID com.audiodestrukt.opto
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Opto
    PRODUCT_NAME "Opto"
    FORMATS ${CD_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx Dynamics
    COPY_PLUGIN_AFTER_BUILD ${CD_COPY_PLUGINS})

target_sources(Opto PRIVATE OptoProcessor.cpp OptoEngine.cpp)
target_include_directories(Opto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# shares the family fonts and knob look with Phys Fuzz
target_link_libraries(Opto
    PRIVATE cd_core PhysFuzzAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(CD_BUILD_TOOLS)
    juce_add_console_app(opto_snapshot PRODUCT_NAME "opto_snapshot")
    target_sources(opto_snapshot PRIVATE tools/snapshot.cpp OptoProcessor.cpp OptoEngine.cpp)
    target_include_directories(opto_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(opto_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(opto_snapshot
        PRIVATE cd_core PhysFuzzAssets juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
