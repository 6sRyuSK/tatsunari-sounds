# resonance-suppressor — Soothe-style dynamic resonance suppressor.
# Composed from factory_core (STFT engine) + factory_ui (shared look & feel).

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml RS_VERSION)
message(STATUS "resonance-suppressor version ${RS_VERSION} (from plugin.toml)")

set(RS_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND RS_FORMATS AU)
endif()

juce_add_plugin(ResonanceSuppressor
  VERSION                     ${RS_VERSION}
  PRODUCT_NAME                "Resonance TatSuppressor"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Rsup
  FORMATS                     ${RS_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx EQ)

target_sources(ResonanceSuppressor PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(ResonanceSuppressor PRIVATE cxx_std_20)

target_compile_definitions(ResonanceSuppressor PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(ResonanceSuppressor
  PRIVATE
    factory_core
    factory_ui
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(resonance_suppressor_dsp_test tests/dsp_test.cpp)
target_link_libraries(resonance_suppressor_dsp_test PRIVATE factory_core)
target_compile_features(resonance_suppressor_dsp_test PRIVATE cxx_std_20)

# Full standard sample-rate matrix, up to 192 kHz (regression coverage for the
# analyser / detector resolution at high sample rates — see tests/dsp_test.cpp).
foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME resonance_suppressor_dsp_${_fs}
           COMMAND resonance_suppressor_dsp_test ${_fs})
endforeach()
