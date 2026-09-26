cmake_minimum_required(VERSION 3.22)

# Circuit Decimator workshop: a shared circuit core plus one folder per product.
#   core/            circuit solver + knob surface (plain C++), engine + UI (JUCE)
#   products/<name>  each product's plugin target(s)
#   tools/ sim/ search/   harnesses, ngspice reference, sound search
# CD_VERSION becomes each plugin's version (CI passes it from the release tag).
set(CD_VERSION "0.1.0" CACHE STRING "Plugin version (x.y.z)")
project(CircuitDecimator VERSION ${CD_VERSION})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CD_COPY_PLUGINS "Install built plugins into this machine's plugin folders" ON)
option(CD_BUILD_TOOLS "Build the dev tools (ff_render, libfuzzface, plugin_render)" ON)

# AU is macOS-only; everything ships VST3 (+ a Standalone app for quick checks).
set(CD_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND CD_PLUGIN_FORMATS AU)
endif()

# ---- plain C++ circuit core (no JUCE): solver, knobs, presets, macros
add_library(cd_circuit INTERFACE)
target_include_directories(cd_circuit INTERFACE core products)

if(CD_BUILD_TOOLS)
    # Offline harness for the realtime solver; sim/compare.py checks it against ngspice.
    add_executable(ff_render tools/ff_render.cpp)
    target_link_libraries(ff_render PRIVATE cd_circuit)

    # Tube mic pre (transformer + 12AX7) on the general engine; sim/tubepre checks it.
    add_executable(tubepre_render tools/tubepre_render.cpp)
    target_link_libraries(tubepre_render PRIVATE cd_circuit)
    target_compile_options(tubepre_render PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # Pentode (6AQ5) against its datasheet operating point, Jacobian check.
    add_executable(pentode_check tools/pentode_check.cpp)
    target_link_libraries(pentode_check PRIVATE cd_circuit)

    # T4 electro-optical cell against UA's published LA-2A attack/release behaviour.
    add_executable(t4_test tools/t4_test.cpp)
    target_link_libraries(t4_test PRIVATE cd_circuit)
    target_compile_options(t4_test PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # LA-2A netlist: operating point, gain, static curve, timing, cost.
    add_executable(la2a_test tools/la2a_test.cpp)
    target_link_libraries(la2a_test PRIVATE cd_circuit)
    target_compile_options(la2a_test PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # LA-2A netlist on sim/la2a/la2a.cir's stimulus (sim/la2a/compare.py).
    add_executable(la2a_render tools/la2a_render.cpp)
    target_link_libraries(la2a_render PRIVATE cd_circuit)
    target_compile_options(la2a_render PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # Any netlist circuit in core/circuit/circuits by name (seout, ...).
    add_executable(circuit_render tools/circuit_render.cpp)
    target_link_libraries(circuit_render PRIVATE cd_circuit)
    target_compile_options(circuit_render PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # The general engine must reproduce the hand-built solvers (transformer, fuzz).
    add_executable(net_selftest tools/net_selftest.cpp)
    target_link_libraries(net_selftest PRIVATE cd_circuit)
    target_compile_options(net_selftest PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)

    # Offline harness for the transformer model; sim/transformer checks it against ngspice.
    add_executable(xfmr_render tools/xfmr_render.cpp)
    target_link_libraries(xfmr_render PRIVATE cd_circuit)

    # C API over the solver + knob mapping, loaded by search/explore.py via ctypes.
    add_library(fuzzface SHARED tools/ff_capi.cpp)
    target_link_libraries(fuzzface PRIVATE cd_circuit)
    target_compile_options(fuzzface PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-O3>)
endif()

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
add_subdirectory(products/iron)

# ---- headless VST3 host: renders a WAV through any built plugin (with automation)
if(CD_BUILD_TOOLS)
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
endif()
