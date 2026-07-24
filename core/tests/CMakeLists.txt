# core/tests — direct, per-primitive spec tests for the shared factory_core DSP
# primitives. These primitives were previously only exercised transitively
# through the plugins that compose them; this suite localises a regression to
# the primitive itself. Same shape as a plugin dsp_test: links only
# factory_core (no JUCE, headless), one CTest case per standard sample rate.

add_executable(core_primitives_test primitives_test.cpp)
target_link_libraries(core_primitives_test PRIVATE factory_core)
target_compile_features(core_primitives_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME core_primitives_${_fs}
           COMMAND core_primitives_test ${_fs})
endforeach()

# LinearRamp — the JUCE-free linear SmoothedValue replacement. reset(sr, seconds)
# is rate-dependent, so it runs across the full sample-rate matrix too.
add_executable(core_linear_ramp_test linear_ramp_test.cpp)
target_link_libraries(core_linear_ramp_test PRIVATE factory_core)
target_compile_features(core_linear_ramp_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME core_linear_ramp_${_fs}
           COMMAND core_linear_ramp_test ${_fs})
endforeach()

# PitchDetector — MPM/NSDF monophonic f0 estimator (pitch-fix). Window lengths
# derive from the rate, so the whole matrix is load-bearing.
add_executable(core_pitch_detector_test pitch_detector_test.cpp)
target_link_libraries(core_pitch_detector_test PRIVATE factory_core)
target_compile_features(core_pitch_detector_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME core_pitch_detector_${_fs}
           COMMAND core_pitch_detector_test ${_fs})
endforeach()

# PsolaShifter — pitch-synchronous OLA shifter (pitch-fix). The identity path
# is an exact-delay contract, so every rate is asserted sample-exactly.
add_executable(core_psola_shifter_test psola_shifter_test.cpp)
target_link_libraries(core_psola_shifter_test PRIVATE factory_core)
target_compile_features(core_psola_shifter_test PRIVATE cxx_std_20)

foreach(_fs 44100 48000 88200 96000 176400 192000)
  add_test(NAME core_psola_shifter_${_fs}
           COMMAND core_psola_shifter_test ${_fs})
endforeach()
