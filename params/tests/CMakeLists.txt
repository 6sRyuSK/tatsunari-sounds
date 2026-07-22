# params/tests — headless unit tests for the shared factory_params model. Same
# shape as core/tests: links ONLY factory_params (no JUCE), so this build also
# PROVES the JUCE-free headers stay JUCE-free — a stray JUCE include would fail to
# compile here. Nothing in factory_params is sample-rate dependent, so there is a
# single CTest case (no per-rate matrix).

add_executable(params_test params_test.cpp)
target_link_libraries(params_test PRIVATE factory_params)
target_compile_features(params_test PRIVATE cxx_std_20)

add_test(NAME params COMMAND params_test)
