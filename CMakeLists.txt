cmake_minimum_required(VERSION 3.22)

# Circuit Decimator workshop: a shared circuit core plus one folder per product.
#   core/            circuit solver + knob surface (plain C++), engine + UI (JUCE)
#   products/<name>  each product's plugin target(s)
#   tools/ sim/ search/   harnesses, ngspice reference, sound search
project(CircuitDecimator VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- plain C++ circuit core (no JUCE): solver, knobs, presets, macros
add_library(cd_circuit INTERFACE)
target_include_directories(cd_circuit INTERFACE core products)

# Offline harness for the realtime solver; sim/compare.py checks it against ngspice.
add_executable(ff_render tools/ff_render.cpp)
target_link_libraries(ff_render PRIVATE cd_circuit)

# C API over the solver + knob mapping, loaded by search/explore.py via ctypes.
add_library(fuzzface SHARED tools/ff_capi.cpp)
target_link_libraries(fuzzface PRIVATE cd_circuit)
target_compile_options(fuzzface PRIVATE -O3)

# ---- JUCE (cloned into ./JUCE: git clone --depth 1 --branch 9.0.2 https://github.com/juce-framework/JUCE.git JUCE)
if(NOT DEFINED JUCE_DIR)
    set(JUCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/JUCE" CACHE PATH "Path to JUCE framework")
endif()
if(NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "JUCE not found at ${JUCE_DIR}")
endif()
add_subdirectory(${JUCE_DIR} JUCE)

# ---- JUCE-side core: compiled into each product with that product's JUCE config
add_library(cd_core INTERFACE)
target_sources(cd_core INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/engine/FuzzEngine.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/ui/CircuitView.cpp)
target_link_libraries(cd_core INTERFACE
    cd_circuit
    juce::juce_audio_utils
    juce::juce_dsp)
target_compile_definitions(cd_core INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_JACK=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

# ---- products
add_subdirectory(products/workbench)
add_subdirectory(products/phys-fuzz)

# ---- headless VST3 host: renders a WAV through any built plugin (with automation)
juce_add_console_app(plugin_render PRODUCT_NAME "plugin_render")
target_sources(plugin_render PRIVATE tools/plugin_render.cpp)
target_compile_definitions(plugin_render PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0)
target_link_libraries(plugin_render
    PRIVATE
        juce::juce_audio_processors_headless
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
