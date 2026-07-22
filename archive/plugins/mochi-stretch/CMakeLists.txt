# mochi-stretch — Red Panda Tensor. Composed from factory_core.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml MOCHI_STRETCH_VERSION)
message(STATUS "mochi-stretch version ${MOCHI_STRETCH_VERSION} (from plugin.toml)")

# VST3 everywhere, AU on Apple, plus a Standalone app for local GUI review.
set(MOCHI_STRETCH_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND MOCHI_STRETCH_FORMATS AU)
endif()

juce_add_plugin(MochiStretch
  VERSION                     ${MOCHI_STRETCH_VERSION}
  PRODUCT_NAME                "Tatsunari Mochi Stretch"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Moch
  FORMATS                     ${MOCHI_STRETCH_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Pitch Shift)

target_sources(MochiStretch PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(MochiStretch PRIVATE cxx_std_20)

target_compile_definitions(MochiStretch PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MochiStretch
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(mochi_stretch_dsp_test tests/dsp_test.cpp)
target_link_libraries(mochi_stretch_dsp_test PRIVATE factory_core)
target_compile_features(mochi_stretch_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME mochi_stretch_dsp_${_fs}
           COMMAND mochi_stretch_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(mochi_stretch_preset_test PRODUCT_NAME "mochi_stretch_preset_test")
target_sources(mochi_stretch_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(mochi_stretch_preset_test PRIVATE Source)
target_compile_features(mochi_stretch_preset_test PRIVATE cxx_std_20)
target_compile_definitions(mochi_stretch_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(mochi_stretch_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME mochi_stretch_preset COMMAND mochi_stretch_preset_test)
