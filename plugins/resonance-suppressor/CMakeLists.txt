# resonance-suppressor — Soothe-style dynamic resonance suppressor.
# Composed from factory_core (STFT engine) + factory_ui (shared look & feel).
#
# 3.0.0 CUTOVER: RS SHIPS CLAP-FIRST. There is deliberately NO juce_add_plugin here
# any more — the shipping build is the make_clapfirst shell (CLAP + wrapper VST3 +
# AUv2-on-Apple, with the Visage editor), assembled from the root CMakeLists when
# RS is configured (see the `_rs_configured` block there ->
# add_subdirectory(plugins/resonance-suppressor/shell), which calls
# factory_clap_plugin). release_plan.py resolves RS's release target from that
# shell/CMakeLists.txt (kind = "clap").
#
# The JUCE ResonanceSuppressorAudioProcessor sources (Source/PluginProcessor.*,
# and the entangled PluginEditor.*) are NO LONGER a shipping plugin — they are
# retained ONLY as the BYTE-EQUIVALENCE ORACLE compiled into the JUCE-linked test
# console apps below (rscore_equiv_test drives the real processor against the
# framework-free rs_core::RsCore; preset_test checks the factory presets + the
# generated APVTS layout against that same processor). Both are headless ctest
# targets, never packaged.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml RS_VERSION)
message(STATUS "resonance-suppressor version ${RS_VERSION} (clap-first; shell assembled from root)")

# ---------------------------------------------------------------- DSP tests
add_executable(resonance_suppressor_dsp_test tests/dsp_test.cpp)
target_link_libraries(resonance_suppressor_dsp_test PRIVATE factory_core)
# Source/ is on the include path ONLY for the JUCE-free Source/DetailParam.h
# (the detail-macro / migration math shared verbatim with PluginProcessor, so
# the headless test asserts the exact shipped mapping). The test still links
# factory_core alone -- DetailParam.h must never grow a JUCE dependency.
target_include_directories(resonance_suppressor_dsp_test PRIVATE Source)
target_compile_features(resonance_suppressor_dsp_test PRIVATE cxx_std_20)

# Full standard sample-rate matrix, up to 192 kHz (regression coverage for the
# analyser / detector resolution at high sample rates — see tests/dsp_test.cpp).
foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME resonance_suppressor_dsp_${_fs}
           COMMAND resonance_suppressor_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(resonance_suppressor_preset_test PRODUCT_NAME "resonance_suppressor_preset_test")
target_sources(resonance_suppressor_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(resonance_suppressor_preset_test PRIVATE Source)
target_compile_features(resonance_suppressor_preset_test PRIVATE cxx_std_20)
target_compile_definitions(resonance_suppressor_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(resonance_suppressor_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    factory_params
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME resonance_suppressor_preset COMMAND resonance_suppressor_preset_test)

# --------------------------------------------------- RsCore equivalence (drift guard)
# JUCE-linked console app: drives the REAL processor (the oracle) and the extracted,
# framework-free rs_core::RsCore side by side with identical input + parameters and
# asserts BYTE-IDENTICAL output + published spectra across the full rate matrix. Links
# JUCE because it instantiates the processor as the oracle; heap-allocates it like
# preset_test. The plugin ROOT dir is on the include path for RsCore.h / ui/RsFeed*.h;
# Source/ for PluginProcessor.h + the shared DetailParam.h. One rate-sweeping case.
juce_add_console_app(resonance_suppressor_rscore_equiv_test PRODUCT_NAME "resonance_suppressor_rscore_equiv_test")
target_sources(resonance_suppressor_rscore_equiv_test PRIVATE
  tests/rscore_equiv_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(resonance_suppressor_rscore_equiv_test PRIVATE Source ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(resonance_suppressor_rscore_equiv_test PRIVATE cxx_std_20)
target_compile_definitions(resonance_suppressor_rscore_equiv_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(resonance_suppressor_rscore_equiv_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    factory_params
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME resonance_suppressor_rscore_equiv COMMAND resonance_suppressor_rscore_equiv_test)
