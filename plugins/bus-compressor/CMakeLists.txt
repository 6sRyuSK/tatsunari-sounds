# bus-compressor — SSL-G-style stereo bus compressor. Composed from factory_core.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml BC_VERSION)
message(STATUS "bus-compressor version ${BC_VERSION} (from plugin.toml)")

set(BC_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND BC_FORMATS AU)
endif()

juce_add_plugin(BusCompressor
  VERSION                     ${BC_VERSION}
  PRODUCT_NAME                "Bus Compressor"
  COMPANY_NAME                "Tatsunari"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Bcmp
  FORMATS                     ${BC_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Dynamics)

target_sources(BusCompressor PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(BusCompressor PRIVATE cxx_std_20)

target_compile_definitions(BusCompressor PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(BusCompressor
  PRIVATE
    factory_core
    factory_ui
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(bus_compressor_dsp_test tests/dsp_test.cpp)
target_link_libraries(bus_compressor_dsp_test PRIVATE factory_core)
target_compile_features(bus_compressor_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME bus_compressor_dsp_${_fs}
           COMMAND bus_compressor_dsp_test ${_fs})
endforeach()
