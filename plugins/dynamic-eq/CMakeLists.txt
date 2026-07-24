# dynamic-eq — Pro-Q-style multi-band dynamic parametric EQ.
#
# 脱JUCE CUTOVER (2.0.0): dynamic-eq SHIPS CLAP-FIRST. There is deliberately NO
# juce_add_plugin here any more — the shipping build is the make_clapfirst shell (CLAP +
# wrapper VST3 + AUv2-on-Apple, with the Visage editor), assembled from the root CMakeLists
# when dynamic-eq is configured (its shell/CMakeLists.txt calls factory_clap_plugin).
# release_plan.py resolves the release target from that shell/CMakeLists.txt (kind = "clap").
#
# The JUCE DynamicEqAudioProcessor sources (Source/PluginProcessor.*, PluginEditor.*) are NO
# LONGER a shipping plugin — they are retained ONLY as the BYTE-EQUIVALENCE ORACLE compiled
# into the JUCE-linked test console apps below (deqcore_equiv_test drives the real processor
# against the framework-free deq_core::DeqCore; preset_test checks the factory presets + the
# generated APVTS layout against that same processor). Both are headless ctest targets, never
# packaged.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml DEQ_VERSION)
message(STATUS "dynamic-eq version ${DEQ_VERSION} (clap-first; shell assembled from root)")

# ---------------------------------------------------------------- DSP tests
add_executable(dynamic_eq_dsp_test tests/dsp_test.cpp)
target_link_libraries(dynamic_eq_dsp_test PRIVATE factory_core)
target_compile_features(dynamic_eq_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME dynamic_eq_dsp_${_fs}
           COMMAND dynamic_eq_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(dynamic_eq_preset_test PRODUCT_NAME "dynamic_eq_preset_test")
target_sources(dynamic_eq_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(dynamic_eq_preset_test PRIVATE Source ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dynamic_eq_preset_test PRIVATE cxx_std_20)
target_compile_definitions(dynamic_eq_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(dynamic_eq_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    factory_params
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME dynamic_eq_preset COMMAND dynamic_eq_preset_test)

# --------------------------------------------------- DeqCore equivalence (drift guard)
# JUCE-linked console app: drives the REAL processor (the oracle) and the extracted,
# framework-free deq_core::DeqCore side by side with identical input + parameters and
# asserts BYTE-IDENTICAL output + published live gains + analyzer rings across the full
# rate matrix. Links JUCE because it instantiates the processor as the oracle; heap-
# allocates it like preset_test. The plugin ROOT dir is on the include path for
# DeqCore.h / DeqParams.h; Source/ for PluginProcessor.h. One rate-sweeping case.
juce_add_console_app(dynamic_eq_deqcore_equiv_test PRODUCT_NAME "dynamic_eq_deqcore_equiv_test")
target_sources(dynamic_eq_deqcore_equiv_test PRIVATE
  tests/deqcore_equiv_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(dynamic_eq_deqcore_equiv_test PRIVATE Source ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(dynamic_eq_deqcore_equiv_test PRIVATE cxx_std_20)
target_compile_definitions(dynamic_eq_deqcore_equiv_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(dynamic_eq_deqcore_equiv_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    factory_params
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME dynamic_eq_deqcore_equiv COMMAND dynamic_eq_deqcore_equiv_test)
