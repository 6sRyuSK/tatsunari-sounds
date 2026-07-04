# multiband-enhancer — Tatsumin Enhancer, a 5-band parallel harmonic enhancer.
# Composed from factory_core (MultibandEnhancer) + factory_ui.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml MBE_VERSION)
message(STATUS "multiband-enhancer version ${MBE_VERSION} (from plugin.toml)")

set(MBE_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND MBE_FORMATS AU)
endif()

juce_add_plugin(MultibandEnhancer
  VERSION                     ${MBE_VERSION}
  PRODUCT_NAME                "Tatsumin Enhancer"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Tmen
  FORMATS                     ${MBE_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx)

target_sources(MultibandEnhancer PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)

target_compile_features(MultibandEnhancer PRIVATE cxx_std_20)

target_compile_definitions(MultibandEnhancer PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MultibandEnhancer
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
add_executable(multiband_enhancer_dsp_test tests/dsp_test.cpp)
target_link_libraries(multiband_enhancer_dsp_test PRIVATE factory_core)
target_compile_features(multiband_enhancer_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME multiband_enhancer_dsp_${_fs}
           COMMAND multiband_enhancer_dsp_test ${_fs})
endforeach()

# ------------------------------------------------------------ preset wiring test
# JUCE-linked console app: builds the processor headless and checks the factory
# preset table against the live parameter layout (IDs exist, values in range,
# Init == defaults, presetIndex round-trips). Rate-independent, one case.
juce_add_console_app(multiband_enhancer_preset_test PRODUCT_NAME "multiband_enhancer_preset_test")
target_sources(multiband_enhancer_preset_test PRIVATE
  tests/preset_test.cpp
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp)
target_include_directories(multiband_enhancer_preset_test PRIVATE Source)
target_compile_features(multiband_enhancer_preset_test PRIVATE cxx_std_20)
target_compile_definitions(multiband_enhancer_preset_test PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)
target_link_libraries(multiband_enhancer_preset_test
  PRIVATE
    factory_core
    factory_ui
    factory_presets
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

add_test(NAME multiband_enhancer_preset COMMAND multiband_enhancer_preset_test)
