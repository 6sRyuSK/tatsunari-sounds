# fuzznari — continuous-morph fuzz (Fuzz Face / Big Muff → gated, self-
# oscillating Fuzz Factory territory). Composed from factory_core; the whole
# DSP (including the RateBracket oversampling) lives in factory_core::FuzzEngine
# so the headless test exercises the production path.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml FUZ_VERSION)
message(STATUS "fuzznari version ${FUZ_VERSION} (from plugin.toml)")

# VST3 everywhere, AU on Apple, plus a Standalone app for local GUI review.
set(FUZ_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND FUZ_FORMATS AU)
endif()

juce_add_plugin(Fuzznari
  VERSION                     ${FUZ_VERSION}
  PRODUCT_NAME                "Fuzznari"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Fuz1
  FORMATS                     ${FUZ_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Distortion)

target_sources(Fuzznari PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(Fuzznari PRIVATE cxx_std_20)

target_compile_definitions(Fuzznari PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(Fuzznari
  PRIVATE
    factory_core
    factory_ui
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(fuzznari_dsp_test tests/dsp_test.cpp)
target_link_libraries(fuzznari_dsp_test PRIVATE factory_core)
target_compile_features(fuzznari_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME fuzznari_dsp_${_fs}
           COMMAND fuzznari_dsp_test ${_fs})
endforeach()
