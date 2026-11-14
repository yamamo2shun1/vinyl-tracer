cmake_minimum_required(VERSION 3.22)
project(VinylTracer VERSION 0.5.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Link the MSVC runtime statically, so the plug-in loads on machines without
# the Visual C++ Redistributable (VCRUNTIME140.dll / MSVCP140.dll).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(VT_BUILD_PLUGIN "Build the JUCE plug-in (needs JUCE)" ON)
option(VT_BUILD_TESTS  "Build the decoder tests and tools"    ON)

# ---------------------------------------------------------------------------
# Core: timecode decoder + scratch player (no JUCE dependency)
add_library(vt_core STATIC
    core/TimecodeDef.cpp
    core/TimecodeDecoder.cpp
    core/ScratchPlayer.cpp)
target_include_directories(vt_core PUBLIC core)
if(MSVC)
    target_compile_options(vt_core PRIVATE /W4 /fp:fast)
else()
    target_compile_options(vt_core PRIVATE -Wall -Wextra -O2)
endif()

# ---------------------------------------------------------------------------
# Tests and command line tools
if(VT_BUILD_TESTS)
    add_executable(vt_test_decoder tests/test_decoder.cpp tests/Synth.cpp)
    target_include_directories(vt_test_decoder PRIVATE tests)
    target_link_libraries(vt_test_decoder PRIVATE vt_core)

    add_executable(vt_test_player tests/test_player.cpp tests/Synth.cpp)
    target_include_directories(vt_test_player PRIVATE tests)
    target_link_libraries(vt_test_player PRIVATE vt_core)

    add_executable(tc_analyze tools/tc_analyze.cpp)
    target_link_libraries(tc_analyze PRIVATE vt_core)
endif()

# ---------------------------------------------------------------------------
# Plug-in
if(VT_BUILD_PLUGIN)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
        add_subdirectory(JUCE)
    else()
        include(FetchContent)
        FetchContent_Declare(JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG        master      # pin a release tag for reproducible builds
            GIT_SHALLOW    TRUE)
        FetchContent_MakeAvailable(JUCE)
    endif()

    set(VT_FORMATS VST3 Standalone)
    if(APPLE)
        list(APPEND VT_FORMATS AU)
    endif()

    juce_add_plugin(VinylTracer
        COMPANY_NAME             "Yamamoto Works"
        BUNDLE_ID                com.yamamotoworks.vinyltracer
        PLUGIN_MANUFACTURER_CODE Ymwk
        PLUGIN_CODE              Vytr
        FORMATS                  ${VT_FORMATS}
        PRODUCT_NAME             "Vinyl Tracer"
        IS_SYNTH                 FALSE
        NEEDS_MIDI_INPUT         FALSE
        NEEDS_MIDI_OUTPUT        FALSE
        IS_MIDI_EFFECT           FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "Vinyl Tracer reads the timecode signal from your audio input."
        COPY_PLUGIN_AFTER_BUILD  FALSE
        VST3_CATEGORIES          Fx)

    target_sources(VinylTracer PRIVATE
        plugin/PluginProcessor.cpp
        plugin/PluginEditor.cpp)

    # Images and fonts built into the plugin (namespace VinylTracerData, header BinaryData.h)
    juce_add_binary_data(VinylTracerData
        NAMESPACE VinylTracerData
        SOURCES   logo/vinyl-tracer-mark-dark.svg
                  fonts/SpaceGrotesk-SemiBold.ttf
                  fonts/JetBrainsMono-Regular.ttf
                  fonts/JetBrainsMono-Bold.ttf)

    target_compile_definitions(VinylTracer PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0)

    target_link_libraries(VinylTracer
        PRIVATE
            vt_core
            VinylTracerData
            juce::juce_audio_utils
            juce::juce_audio_formats
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags
            juce::juce_recommended_warning_flags)
endif()
