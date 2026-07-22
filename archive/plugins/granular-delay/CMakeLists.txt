# granular-delay — granular cloud delay with pitch, tempo-sync and LFO.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml GD_VERSION)
message(STATUS "granular-delay version ${GD_VERSION} (from plugin.toml)")

set(GD_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND GD_FORMATS AU)
endif()

juce_add_plugin(GranularDelay
  VERSION                     ${GD_VERSION}
  PRODUCT_NAME                "Tatsunular Delay"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Grnd
  FORMATS                     ${GD_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Delay)

target_sources(GranularDelay PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(GranularDelay PRIVATE cxx_std_20)

target_compile_definitions(GranularDelay PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(GranularDelay
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(granular_delay_dsp_test tests/dsp_test.cpp)
target_link_libraries(granular_delay_dsp_test PRIVATE factory_core)
target_compile_features(granular_delay_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME granular_delay_dsp_${_fs}
           COMMAND granular_delay_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(granular_delay_preset_test PRODUCT_NAME "granular_delay_preset_test")
target_sources(granular_delay_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(granular_delay_preset_test PRIVATE Source)
target_compile_features(granular_delay_preset_test PRIVATE cxx_std_20)
target_compile_definitions(granular_delay_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(granular_delay_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME granular_delay_preset COMMAND granular_delay_preset_test)
