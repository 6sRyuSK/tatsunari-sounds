# pitch-fix — Pitch TatFixer (CLAP-FIRST plugin).
#
# The SHIPPING binaries (CLAP + wrapper VST3, AUv2 on Apple) are assembled by
# shell/CMakeLists.txt via factory_clap_plugin, which the ROOT CMakeLists adds
# whenever this plugin is in the configured set (any plugin carrying a
# shell/CMakeLists.txt joins the clap-first assembly). There is NO JUCE target
# here — this file registers only the headless tests.

# Headless DSP spec tests: links ONLY factory_core (no JUCE/CLAP/host); one
# CTest case per standard sample rate (argv[1] = rate, none = full matrix).
add_executable(pitch_fix_dsp_test tests/dsp_test.cpp)
target_link_libraries(pitch_fix_dsp_test PRIVATE factory_core)
target_include_directories(pitch_fix_dsp_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pitch_fix_dsp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME pitch_fix_dsp_${_fs}
           COMMAND pitch_fix_dsp_test ${_fs})
endforeach()

# Parameter-table + factory-preset wiring test (headless, JUCE-free; nothing
# here is sample-rate dependent, so a single CTest case).
add_executable(pitch_fix_preset_test tests/preset_test.cpp)
target_link_libraries(pitch_fix_preset_test PRIVATE factory_params factory_presets)
target_include_directories(pitch_fix_preset_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(pitch_fix_preset_test PRIVATE cxx_std_20)
add_test(NAME pitch_fix_preset COMMAND pitch_fix_preset_test)
