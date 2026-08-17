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
# Web UI (ui/ = Vite + React + TypeScript SPA hosted in a WebBrowserComponent)
#
#   ON  : npm ci + npm run build in ui/, zip dist/, bake it into the binary and
#         serve it from the resource provider at juce://juce.backend/.
#   OFF : no Node required, nothing baked in; the editor loads the Vite dev
#         server (http://localhost:5173) instead. `npm run dev` in ui/.
# ---------------------------------------------------------------------------
option(TUBAMP_BUNDLED_UI "Bundle the built web UI into the plugin binary" ON)

# Where the editor loads its HTML from. Defaults to the complement of
# TUBAMP_BUNDLED_UI, but it is its own knob: -DTUBAMP_BUNDLED_UI=OFF
# -DTUBAMP_WEBUI_DEV=OFF gives a Node-free build that still points at the baked-in
# resource provider (and so shows the "UI bundle missing" page rather than silently
# depending on a dev server that is not running).
if(TUBAMP_BUNDLED_UI)
    set(tubamp_webui_dev_default OFF)
else()
    set(tubamp_webui_dev_default ON)
endif()
option(TUBAMP_WEBUI_DEV "Load the Vite dev server (http://localhost:5173) instead of the bundled UI"
       ${tubamp_webui_dev_default})

# TONE3000 OAuth client_id baked into the binary so users need zero setup.
# Publishable keys (t3k_pub_...) are designed to be safe client-side — they only
# identify the app; user auth still happens per-user via PKCE. A key entered in
# Settings (t3kConfigure) overrides this at runtime. Rotate via the cache entry
# or the TUBAMP_T3K_CLIENT_ID environment variable at configure time.
set(tubamp_t3k_client_id_default "t3k_pub_U1hZrLKf19O7-jl0wTbPyhERpluPpFcj")
if(DEFINED ENV{TUBAMP_T3K_CLIENT_ID})
    set(tubamp_t3k_client_id_default "$ENV{TUBAMP_T3K_CLIENT_ID}")
endif()
set(TUBAMP_T3K_CLIENT_ID "${tubamp_t3k_client_id_default}" CACHE STRING
    "TONE3000 publishable key (OAuth client_id) baked into the plugin")

# A dev-server build is useless to a host and must never overwrite the installed
# shipping component, so only release-shaped builds copy themselves into
# ~/Library/Audio/Plug-Ins/Components. juce_add_plugin consumes this at call time.
if(TUBAMP_WEBUI_DEV)
    set(tubamp_copy_after_build FALSE)
else()
    set(tubamp_copy_after_build TRUE)
endif()

set(webui_dir "${CMAKE_CURRENT_SOURCE_DIR}/ui")

# "juce-framework-frontend" is not published on npm and lives inside the
# FetchContent'd (gitignored) JUCE checkout, so a package.json "file:" dependency
# would break a clean `npm ci`. Vendor it into the Vite project at configure time
# instead; both `npm run dev` and `npm run build` then resolve ./juce/index.js.
if(EXISTS "${webui_dir}/package.json")
    file(COPY "${JUCE_MODULES_DIR}/juce_gui_extra/native/javascript/"
         DESTINATION "${webui_dir}/src/juce/")
endif()

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
juce_add_plugin(tubamp
    COMPANY_NAME                "t0audio"
    BUNDLE_ID                   "com.t0audio.tubamp"
    # Frozen: hosts identify the AU as aufx/Tamp/Tuba (saved sessions, auval).
    # The display name above can change; this code cannot.
    PLUGIN_MANUFACTURER_CODE    Tuba
    PLUGIN_CODE                 Tamp
    FORMATS                     AU Standalone
    AU_MAIN_TYPE                kAudioUnitType_Effect
    PRODUCT_NAME                "tubamp"
    COPY_PLUGIN_AFTER_BUILD     ${tubamp_copy_after_build}
    AU_COPY_DIR                 "$ENV{HOME}/Library/Audio/Plug-Ins/Components"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT  "tubamp needs audio input for the standalone app."
    # Hosts third-party AudioUnit effects in the chain's FX slots (src/dsp/FxHost.h).
    # The keyword form is required over a bare JUCE_PLUGINHOST_AU define: it also links
    # CoreAudioKit and AudioUnit, which the AU/Standalone link steps need.
    PLUGINHOST_AU               TRUE
    # No-op on macOS (the module default already enables WebKit) but required the
    # day a Linux/Windows target appears.
    NEEDS_WEB_BROWSER           TRUE)

file(GLOB_RECURSE tubamp_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# DSP + state + library only: everything the headless console targets can compile.
# WebEditor pulls in WebBrowserComponent / relays / Web*ParameterAttachment, all of
# which live inside `#if JUCE_WEB_BROWSER` blocks and simply do not exist there.
set(tubamp_core_sources ${tubamp_sources})
list(FILTER tubamp_core_sources EXCLUDE REGEX "/src/WebEditor\\.(cpp|h)$")

target_sources(tubamp PRIVATE ${tubamp_sources})
target_include_directories(tubamp PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

# ---------------------------------------------------------------------------
# Web UI: npm build -> ui/dist -> zip -> binary data
# ---------------------------------------------------------------------------
set(TUBAMP_WEBUI_ZIP_PREFIX "dist/")

if(TUBAMP_BUNDLED_UI)
    if(NOT EXISTS "${webui_dir}/package.json")
        message(FATAL_ERROR
            "TUBAMP_BUNDLED_UI=ON but ${webui_dir}/package.json is missing.\n"
            "Scaffold ui/ first, or configure with -DTUBAMP_BUNDLED_UI=OFF to load "
            "the Vite dev server (no Node needed at build time).")
    endif()

    find_program(NPM_EXECUTABLE npm REQUIRED)

    set(webui_zip "${CMAKE_BINARY_DIR}/tubamp_webui.zip")

    # Only the source trees are globbed recursively. GLOB_RECURSE with a literal
    # (wildcard-free) trailing filename walks the WHOLE subtree looking for that
    # name, so "${webui_dir}/package.json" would match every
    # ui/node_modules/**/package.json and "${webui_dir}/index.html" would match the
    # command's own output ui/dist/index.html — a self-referential dependency that
    # Makefiles drop with a "Circular ... dependency" warning and Ninja rejects
    # outright. Plain GLOB does not recurse, so the top-level literals stay literal.
    # (public/ need not exist; a glob that matches nothing contributes nothing.)
    file(GLOB_RECURSE webui_src_inputs CONFIGURE_DEPENDS
        "${webui_dir}/src/*"
        "${webui_dir}/public/*")
    file(GLOB webui_root_inputs CONFIGURE_DEPENDS
        "${webui_dir}/index.html"
        "${webui_dir}/package.json"
        "${webui_dir}/package-lock.json"
        "${webui_dir}/vite.config.ts"
        "${webui_dir}/tsconfig*.json")
    set(webui_inputs ${webui_src_inputs} ${webui_root_inputs})
    list(FILTER webui_inputs EXCLUDE REGEX "/node_modules/")

    # OUTPUT-based so incremental builds skip npm entirely when nothing changed.
    add_custom_command(
        OUTPUT  "${webui_dir}/dist/index.html"
        COMMAND "${NPM_EXECUTABLE}" ci --prefer-offline --no-audit
        COMMAND "${NPM_EXECUTABLE}" run build
        WORKING_DIRECTORY "${webui_dir}"
        DEPENDS ${webui_inputs}
        COMMENT "Building tubamp web UI (vite)"
        VERBATIM)

    # `cmake -E tar` stores paths relative to WORKING_DIRECTORY, so entries come out
    # as "dist/index.html" — the resource provider prepends the same prefix.
    add_custom_command(
        OUTPUT  "${webui_zip}"
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${webui_zip}" --format=zip "dist"
        WORKING_DIRECTORY "${webui_dir}"
        DEPENDS "${webui_dir}/dist/index.html"
        COMMENT "Zipping tubamp web UI"
        VERBATIM)

    add_custom_target(tubamp_webui DEPENDS "${webui_zip}")

    # Vite emits content-hashed filenames, so per-file binary data would churn the
    # generated symbol names on every build; the zip keeps one stable symbol
    # (webui::tubamp_webui_zip / ...zipSize).
    juce_add_binary_data(TubampWebUiData
        HEADER_NAME WebUiData.h
        NAMESPACE   webui
        SOURCES     "${webui_zip}")
    add_dependencies(TubampWebUiData tubamp_webui)

    target_link_libraries(tubamp PRIVATE TubampWebUiData)
endif()

target_compile_definitions(tubamp PUBLIC
    JUCE_WEB_BROWSER=1
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    TUBAMP_HEADLESS=0
    TUBAMP_BUNDLED_UI=$<BOOL:${TUBAMP_BUNDLED_UI}>
    TUBAMP_WEBUI_DEV=$<BOOL:${TUBAMP_WEBUI_DEV}>
    TUBAMP_WEBUI_ZIP_PREFIX="${TUBAMP_WEBUI_ZIP_PREFIX}"
    TUBAMP_T3K_CLIENT_ID="${TUBAMP_T3K_CLIENT_ID}")

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
    ${tubamp_core_sources})
target_include_directories(tubamp_smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tubamp_smoke PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1
    TUBAMP_HEADLESS=1
    TUBAMP_T3K_CLIENT_ID="${TUBAMP_T3K_CLIENT_ID}")
target_link_libraries(tubamp_smoke PRIVATE
    nam_core
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# ---------------------------------------------------------------------------
# Reverb measurement rig (docs/REVERB.md §7 Stage 0, §8): renders wet-only IRs out of
# ReverbEngine and gates them on the §8 metric table plus §3.12's Reverse gates. Only
# the engine is linked -- it is self-contained (juce_dsp only, TUBAMP_HEADLESS-clean),
# so the probe needs neither the processor nor nam_core.
# ---------------------------------------------------------------------------
juce_add_console_app(tubamp_verbprobe PRODUCT_NAME "tubamp_verbprobe")
target_sources(tubamp_verbprobe PRIVATE
    tools/VerbProbe.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/src/dsp/ReverbEngine.cpp")
target_include_directories(tubamp_verbprobe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tubamp_verbprobe PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STANDALONE_APPLICATION=1
    TUBAMP_HEADLESS=1)
target_link_libraries(tubamp_verbprobe PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_recommended_config_flags)

# ---------------------------------------------------------------------------
# Headless probe for the external-AU slots: the only automated coverage of the
# code paths that need a real AudioUnit. Unlike tubamp_smoke this one IS built with
# hosting enabled, so it can enumerate the machine's plugins, load one into a slot
# and verify the audio, latency and state behaviour end to end without a DAW.
# ---------------------------------------------------------------------------
if(APPLE)
    juce_add_console_app(tubamp_fxprobe PRODUCT_NAME "tubamp_fxprobe")
    target_sources(tubamp_fxprobe PRIVATE
        tools/FxProbe.cpp
        ${tubamp_core_sources})
    target_include_directories(tubamp_fxprobe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(tubamp_fxprobe PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_AU=1
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_STANDALONE_APPLICATION=1
        TUBAMP_HEADLESS=1
        TUBAMP_T3K_CLIENT_ID="${TUBAMP_T3K_CLIENT_ID}")
    target_link_libraries(tubamp_fxprobe PRIVATE
        nam_core
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        "-framework AudioUnit"
        "-framework CoreAudioKit")
endif()

# Note: the former `tubamp_uishot` target (tools/UiSnapshot.cpp) is gone. Its whole
# job was editor->createComponentSnapshot(), and an NSViewComponent-hosted WKWebView
# does not render into a JUCE Graphics context. UI iteration now happens against
# `npm run dev` + the mock bridge in a normal browser.

# ---------------------------------------------------------------------------
# THROWAWAY prototype: in-plugin TONE3000 browser UI options (3 variants).
# Standalone GUI app, no audio, no network — stub catalog data only. Not part
# of the shipping plugin; see prototype/t3k-browser/README.md.
# Run: prototype/t3k-browser/run.sh [--variant=A|B|C]
# ---------------------------------------------------------------------------
# Guarded twice over: this throwaway prototype directory is not present in every
# checkout (it predates the DSP/UI rewrite and isn't part of the shipping
# plugin), AND it links src/ui/TubampLookAndFeel.cpp, which the WebView rewrite
# deleted along with the rest of src/ui/. A checkout that still has the
# prototype would therefore fail to configure on a missing source file — skip
# the target unless BOTH halves are actually present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prototype/t3k-browser/Main.cpp"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/TubampLookAndFeel.cpp")
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
