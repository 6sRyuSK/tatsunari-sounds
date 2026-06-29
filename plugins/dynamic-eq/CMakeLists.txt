# dynamic-eq — multi-band dynamic parametric EQ. Composed from factory_core.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml DEQ_VERSION)
message(STATUS "dynamic-eq version ${DEQ_VERSION} (from plugin.toml)")

set(DEQ_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND DEQ_FORMATS AU)
endif()

juce_add_plugin(DynamicEq
  VERSION                     ${DEQ_VERSION}
  PRODUCT_NAME                "Dynamic Parametric EQ"
  COMPANY_NAME                "Tatsunari"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Dyeq
  FORMATS                     ${DEQ_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx EQ)

target_sources(DynamicEq PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(DynamicEq PRIVATE cxx_std_20)

target_compile_definitions(DynamicEq PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(DynamicEq
  PRIVATE
    factory_core
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(dynamic_eq_dsp_test tests/dsp_test.cpp)
target_link_libraries(dynamic_eq_dsp_test PRIVATE factory_core)
target_compile_features(dynamic_eq_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 96000)
  add_test(NAME dynamic_eq_dsp_${_fs}
           COMMAND dynamic_eq_dsp_test ${_fs})
endforeach()
