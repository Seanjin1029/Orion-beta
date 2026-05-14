cmake_minimum_required(VERSION 3.22)
project(AbletonAIPlugin VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Download JUCE automatically
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        7.0.12
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(AbletonAIPlugin
    COMPANY_NAME                "Independent"
    PLUGIN_MANUFACTURER_CODE    Abai
    PLUGIN_CODE                 Abai
    FORMATS                     VST3 AU Standalone
    PRODUCT_NAME                "Ableton AI Plugin"
    DESCRIPTION                 "Generate custom audio effects with Claude AI"
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    VST3_CATEGORIES             "Fx"
    AU_MAIN_TYPE                "kAudioUnitType_Effect"
)

target_sources(AbletonAIPlugin PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(AbletonAIPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0          # use macOS NSURLSession, not curl
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
)

# ---- libfaust (LLVM JIT) -----------------------------------------------
set(FAUST_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../deps/faust")

target_include_directories(AbletonAIPlugin PRIVATE
    "${FAUST_DEPS_DIR}/include"
)

target_link_libraries(AbletonAIPlugin
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        "${FAUST_DEPS_DIR}/lib/libfaustwithllvm.a"
        # macOS system frameworks required by LLVM
        "-framework CoreFoundation"
        "-framework CoreServices"
        "-framework Security"
        "-lz"
        "-lncurses"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Path to bundled FAUST standard libraries — written into the binary at
# compile time so the plugin can find them without installation.
target_compile_definitions(AbletonAIPlugin PRIVATE
    FAUST_LIBRARIES_PATH="${FAUST_DEPS_DIR}/libraries"
)

# Generates JuceHeader.h — must come after target_link_libraries
juce_generate_juce_header(AbletonAIPlugin)
