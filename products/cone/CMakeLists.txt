# Cone: a guitar speaker and cabinet, physically modelled (driver circuit + cone breakup + near-field mic IR),
# built from a netlist on the general engine (core/circuit/Circuit.h).
# The plugin code is this product's unique ID in hosts: fix it before release.
juce_add_plugin(Cone
    COMPANY_NAME "Audiodestrukt"
    BUNDLE_ID com.audiodestrukt.cone
    PLUGIN_MANUFACTURER_CODE Adst
    PLUGIN_CODE Cone
    PRODUCT_NAME "Cone"
    FORMATS ${CD_PLUGIN_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx
    COPY_PLUGIN_AFTER_BUILD ${CD_COPY_PLUGINS})

target_sources(Cone PRIVATE ConeProcessor.cpp ConeEngine.cpp)
target_include_directories(Cone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
# shares the family fonts and knob look with Phys Fuzz
target_link_libraries(Cone
    PRIVATE cd_core PhysFuzzAssets
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if(CD_BUILD_TOOLS)
    juce_add_console_app(cone_snapshot PRODUCT_NAME "cone_snapshot")
    target_sources(cone_snapshot PRIVATE tools/snapshot.cpp ConeProcessor.cpp ConeEngine.cpp)
    target_include_directories(cone_snapshot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(cone_snapshot PRIVATE JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(cone_snapshot
        PRIVATE cd_core PhysFuzzAssets juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endif()
