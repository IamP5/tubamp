cmake_minimum_required(VERSION 3.24)

project(tubamp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

include(FetchContent)

# ---------------------------------------------------------------------------
# JUCE 8 (final 8.x release)
# ---------------------------------------------------------------------------
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.15
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# NeuralAmpModelerCore v0.5.4 (+ Eigen and AudioDSPTools submodules)
# No upstream library target exists: glob the sources into our own static lib.
# ---------------------------------------------------------------------------
FetchContent_Declare(NeuralAmpModelerCore
    GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
    GIT_TAG        v0.5.4
    GIT_SUBMODULES "Dependencies/eigen" "Dependencies/AudioDSPTools"
    GIT_SHALLOW    FALSE
    SOURCE_SUBDIR  "does-not-exist-skip-add-subdirectory")
FetchContent_MakeAvailable(NeuralAmpModelerCore)
set(nam_dir "${neuralampmodelercore_SOURCE_DIR}")

file(GLOB nam_core_sources
    "${nam_dir}/NAM/*.cpp"
    "${nam_dir}/NAM/wavenet/*.cpp")

add_library(nam_core STATIC ${nam_core_sources})
target_include_directories(nam_core SYSTEM PUBLIC
    "${nam_dir}"
    "${nam_dir}/Dependencies/nlohmann"
    "${nam_dir}/Dependencies/eigen"
    "${nam_dir}/Dependencies/AudioDSPTools")
target_compile_features(nam_core PUBLIC cxx_std_20)
# Note: do NOT add DSP_SAMPLE_FLOAT here — AudioDSPTools' NoiseGate.h is internally
# inconsistent under DSP_SAMPLE=float (hardcoded double member vs DSP_SAMPLE accessors).
# The gate wrapper in FxBlocks converts float<->double through scratch buffers instead.
target_compile_definitions(nam_core PUBLIC
    NAM_SAMPLE_FLOAT=1
    NAM_ENABLE_A2_FAST)

# AudioDSPTools sources needed for dsp::ResamplingContainer / ImpulseResponse / wav
file(GLOB adt_sources "${nam_dir}/Dependencies/AudioDSPTools/dsp/*.cpp")
target_sources(nam_core PRIVATE ${adt_sources})

# NAM architectures self-register via file-scope statics that nothing references
# (e.g. SlimmableContainer in container.cpp); force-load the whole archive into every
# final link so the linker cannot drop them. This must be an INTERFACE option here,
# not a PRIVATE option on the plugin target: juce_add_plugin's shared-code target is
# itself a static library, so link options set on it never reach the actual AU /
# Standalone link steps.
if(APPLE)
    target_link_options(nam_core INTERFACE "LINKER:-force_load,$<TARGET_FILE:nam_core>")
endif()

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(tubamp
    COMPANY_NAME                "Tuba"
    BUNDLE_ID                   "com.tuba.tubamp"
    PLUGIN_MANUFACTURER_CODE    Tuba
    PLUGIN_CODE                 Tamp
    FORMATS                     AU Standalone
    AU_MAIN_TYPE                kAudioUnitType_Effect
    PRODUCT_NAME                "tubamp"
    COPY_PLUGIN_AFTER_BUILD     TRUE
    AU_COPY_DIR                 "$ENV{HOME}/Library/Audio/Plug-Ins/Components"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "tubamp needs audio input for the standalone app."
    NEEDS_WEB_BROWSER           FALSE)

file(GLOB_RECURSE tubamp_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

target_sources(tubamp PRIVATE ${tubamp_sources})
target_include_directories(tubamp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(tubamp PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(tubamp
    PRIVATE
        nam_core
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------
# Headless end-to-end smoke test: real processor, real model, no editor.
# ---------------------------------------------------------------------------
juce_add_console_app(tubamp_smoke PRODUCT_NAME "tubamp_smoke")
target_sources(tubamp_smoke PRIVATE
    tests/ProcessorSmokeTest.cpp
    ${tubamp_sources})
target_include_directories(tubamp_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tubamp_smoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(tubamp_smoke PRIVATE
    nam_core
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# ---------------------------------------------------------------------------
# Headless UI snapshot: renders the real editor to a PNG (no window, no screen
# permissions) for iterating on the UI outside a DAW. tools/UiSnapshot.cpp.
# ---------------------------------------------------------------------------
juce_add_console_app(tubamp_uishot PRODUCT_NAME "tubamp_uishot")
target_sources(tubamp_uishot PRIVATE
    tools/UiSnapshot.cpp
    ${tubamp_sources})
target_include_directories(tubamp_uishot PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tubamp_uishot PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(tubamp_uishot PRIVATE
    nam_core
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# ---------------------------------------------------------------------------
# THROWAWAY prototype: in-plugin TONE3000 browser UI options (3 variants).
# Standalone GUI app, no audio, no network — stub catalog data only. Not part
# of the shipping plugin; see prototype/t3k-browser/README.md.
# Run: prototype/t3k-browser/run.sh [--variant=A|B|C]
# ---------------------------------------------------------------------------
# Guarded: this throwaway prototype directory is not present in every
# checkout (it predates the DSP/UI rewrite and isn't part of the shipping
# plugin). Skip the target entirely rather than break `cmake --build`.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prototype/t3k-browser/Main.cpp")
juce_add_gui_app(tubamp_proto_t3k_browser PRODUCT_NAME "tubamp_proto_t3k_browser")
target_sources(tubamp_proto_t3k_browser PRIVATE
    prototype/t3k-browser/Main.cpp
    prototype/t3k-browser/Widgets.cpp
    prototype/t3k-browser/VariantA.cpp
    prototype/t3k-browser/VariantB.cpp
    prototype/t3k-browser/VariantC.cpp
    src/ui/TubampLookAndFeel.cpp)
target_include_directories(tubamp_proto_t3k_browser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/prototype/t3k-browser")
target_compile_definitions(tubamp_proto_t3k_browser PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(tubamp_proto_t3k_browser PRIVATE
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
endif()
