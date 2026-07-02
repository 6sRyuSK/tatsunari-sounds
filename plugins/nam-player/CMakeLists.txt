# nam-player — Neural Amp Modeler (NAM) player/loader: 3 NAM models + 1 IR with
# per-slot Series/Parallel routing, composed from factory_core. Links nam_core
# (NeuralAmpModelerCore, the factory's first external dependency — see
# cmake/NamCore.cmake) for model inference.

factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/plugin.toml NAMP_VERSION)
message(STATUS "nam-player version ${NAMP_VERSION} (from plugin.toml)")

set(NAMP_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND NAMP_FORMATS AU)
endif()

include(NamCore)   # defines the nam_core OBJECT library (FetchContent NAM + Eigen)

juce_add_plugin(NamPlayer
  VERSION                     ${NAMP_VERSION}
  PRODUCT_NAME                "Super Tatsunari NAM Player"
  COMPANY_NAME                "Tatsunari Sounds"
  PLUGIN_MANUFACTURER_CODE    Ttsn
  PLUGIN_CODE                 Nam1
  FORMATS                     ${NAMP_FORMATS}
  IS_SYNTH                    FALSE
  NEEDS_MIDI_INPUT            FALSE
  NEEDS_MIDI_OUTPUT           FALSE
  IS_MIDI_EFFECT              FALSE
  COPY_PLUGIN_STEP            FALSE
  VST3_CATEGORIES             Fx Distortion)

target_sources(NamPlayer PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/NamModel.cpp)

target_compile_features(NamPlayer PRIVATE cxx_std_20)

target_compile_definitions(NamPlayer PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(NamPlayer
  PRIVATE
    factory_core
    factory_ui
    nam_core
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------- DSP tests
# Headless: routing engine, FFT convolver, resampler latency. Links factory_core
# only — no JUCE, no NeuralAmpModelerCore (the NAM inference is mocked behind
# factory_core::MonoProcessor).
add_executable(nam_player_dsp_test tests/dsp_test.cpp)
target_link_libraries(nam_player_dsp_test PRIVATE factory_core)
target_compile_features(nam_player_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME nam_player_dsp_${_fs}
           COMMAND nam_player_dsp_test ${_fs})
endforeach()
