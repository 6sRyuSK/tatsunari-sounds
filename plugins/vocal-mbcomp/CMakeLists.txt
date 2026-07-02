# vocal-mbcomp — vocal-tuned 3-band compressor. Composed from factory_core.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml VMB_VERSION)
message(STATUS "vocal-mbcomp version ${VMB_VERSION} (from plugin.toml)")

set(VMB_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND VMB_FORMATS AU)
endif()

juce_add_plugin(VocalMbComp
  VERSION                     ${VMB_VERSION}
  PRODUCT_NAME                "Multi Tatsunari Comp"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Vmbc
  FORMATS                     ${VMB_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Dynamics)

target_sources(VocalMbComp PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(VocalMbComp PRIVATE cxx_std_20)

target_compile_definitions(VocalMbComp PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(VocalMbComp
  PRIVATE
    factory_core
    factory_ui
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(vocal_mbcomp_dsp_test tests/dsp_test.cpp)
target_link_libraries(vocal_mbcomp_dsp_test PRIVATE factory_core)
target_compile_features(vocal_mbcomp_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME vocal_mbcomp_dsp_${_fs}
           COMMAND vocal_mbcomp_dsp_test ${_fs})
endforeach()
