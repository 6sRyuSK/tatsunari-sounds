# single-band-eq — one RBJ peaking band, stereo. Composed from factory_core.

# plugin.toml is the single source of truth for the version.
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml SBEQ_VERSION)
message(STATUS "single-band-eq version ${SBEQ_VERSION} (from plugin.toml)")

# AU only builds on Apple platforms.
set(SBEQ_FORMATS VST3)
if(APPLE)
  list(APPEND SBEQ_FORMATS AU)
endif()

juce_add_plugin(SingleBandEQ
  VERSION                     ${SBEQ_VERSION}
  PRODUCT_NAME                "Single-Band EQ"
  COMPANY_NAME                "Tatsunari"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Sbeq
  FORMATS                     ${SBEQ_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx EQ)

target_sources(SingleBandEQ PRIVATE
  Source/PluginProcessor.cpp)

target_compile_features(SingleBandEQ PRIVATE cxx_std_20)

target_compile_definitions(SingleBandEQ PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(SingleBandEQ
  PRIVATE
    factory_core
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
add_executable(single_band_eq_dsp_test tests/dsp_test.cpp)
target_link_libraries(single_band_eq_dsp_test PRIVATE factory_core)
target_compile_features(single_band_eq_dsp_test PRIVATE cxx_std_20)

# Oracle + invariants at each required sample rate.
foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME single_band_eq_dsp_${_fs}
           COMMAND single_band_eq_dsp_test ${_fs})
endforeach()
