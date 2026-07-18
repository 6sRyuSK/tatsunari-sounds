# presets/tests — headless unit tests for the framework-free preset model
# successors (StateCodec / PresetSession / UserPresetStoreFs). Same shape as
# core/tests and params/tests: links ONLY factory_presets + factory_params (no
# framework), so this build also PROVES those new headers stay framework-free — a
# stray framework include would fail to compile here. Nothing here is sample-rate
# dependent, so there is a single CTest case (no per-rate matrix).

add_executable(presets_test presets_test.cpp)
target_link_libraries(presets_test PRIVATE factory_presets factory_params)
target_compile_features(presets_test PRIVATE cxx_std_20)

add_test(NAME presets COMMAND presets_test)
