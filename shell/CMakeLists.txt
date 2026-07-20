# shell/ — factory_clap_shell: the reusable, framework-free CLAP glue over the
# CLAP C API. Reached ONLY under FACTORY_RS_CLAP (the root add_subdirectory(shell)
# is gated), so nothing here is configured by the default/shipping build.

# Bring in the make_clapfirst wrapper macro; including it also FetchContent's the
# pinned CLAP + VST3 + clap-wrapper SDKs (guarded to run once).
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(FactoryClapPlugin)

# The generic shell: non-templated bridges compiled here; the templated
# ClapShellPlugin<Policy> is header-only and pulled in by each plugin's impl.
add_library(factory_shell STATIC
  src/ClapParamBridge.cpp
  src/ClapStateBridge.cpp)

target_include_directories(factory_shell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# clap (C API headers) + the framework-free factory value/preset models. No
# audio-framework dependency.
target_link_libraries(factory_shell
  PUBLIC
    clap
    factory_params
    factory_presets)

target_compile_features(factory_shell PUBLIC cxx_std_17)
set_target_properties(factory_shell PROPERTIES POSITION_INDEPENDENT_CODE ON)
