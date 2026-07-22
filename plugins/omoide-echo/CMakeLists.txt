# omoide-echo — Chase Bliss Habit. Composed from factory_core.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml OMOIDE_ECHO_VERSION)
message(STATUS "omoide-echo version ${OMOIDE_ECHO_VERSION} (from plugin.toml)")

# VST3 everywhere, AU on Apple, plus a Standalone app for local GUI review.
set(OMOIDE_ECHO_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND OMOIDE_ECHO_FORMATS AU)
endif()

juce_add_plugin(OmoideEcho
  VERSION                     ${OMOIDE_ECHO_VERSION}
  PRODUCT_NAME                "Tatsunari Omoide Echo"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Omoi
  FORMATS                     ${OMOIDE_ECHO_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Delay)

target_sources(OmoideEcho PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(OmoideEcho PRIVATE cxx_std_20)

target_compile_definitions(OmoideEcho PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(OmoideEcho
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
add_executable(omoide_echo_dsp_test tests/dsp_test.cpp)
target_link_libraries(omoide_echo_dsp_test PRIVATE factory_core)
target_compile_features(omoide_echo_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME omoide_echo_dsp_${_fs}
           COMMAND omoide_echo_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(omoide_echo_preset_test PRODUCT_NAME "omoide_echo_preset_test")
target_sources(omoide_echo_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(omoide_echo_preset_test PRIVATE Source)
target_compile_features(omoide_echo_preset_test PRIVATE cxx_std_20)
target_compile_definitions(omoide_echo_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(omoide_echo_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME omoide_echo_preset COMMAND omoide_echo_preset_test)
