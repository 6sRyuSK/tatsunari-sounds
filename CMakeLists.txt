cmake_minimum_required(VERSION 3.22)   # JUCE 8 requires CMake >= 3.22

project(PluginFactory VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Sources are UTF-8; tell MSVC so it doesn't warn (C4819) under non-UTF-8 locales.
if(MSVC)
  add_compile_options(/utf-8)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(FactoryHelpers)

# --- JUCE (pinned; bump to the latest 8.0.x as you like) ---
include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.13
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(JUCE)

# --- Shared DSP core (header-only for now; plugins compose from this) ---
add_library(factory_core INTERFACE)
target_include_directories(factory_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_features(factory_core INTERFACE cxx_std_20)

# --- Shared UI design system (header-only; the factory's look & feel) ---
add_library(factory_ui INTERFACE)
target_include_directories(factory_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ui/include)
target_compile_features(factory_ui INTERFACE cxx_std_20)

# --- Shared factory-preset model (header-only) ---
# PresetBank.h is JUCE-independent; ProgramAdapter.h rides the JUCE program API.
add_library(factory_presets INTERFACE)
target_include_directories(factory_presets INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/presets/include)
target_compile_features(factory_presets INTERFACE cxx_std_20)

enable_testing()

# --- Auto-include every plugins/<slug> that has a CMakeLists.txt ---
# FACTORY_PLUGINS narrows the build to a subset of slugs (comma- or
# semicolon-separated). Empty (the default) configures everything. CI passes
# the plugins a PR touches so unaffected binaries are not rebuilt; anything
# that could affect all plugins (core/ ui/ cmake/ this file) keeps it empty.
set(FACTORY_PLUGINS "" CACHE STRING "Plugin slugs to configure, comma/semicolon separated (empty = all)")
string(REPLACE "," ";" _factory_plugins "${FACTORY_PLUGINS}")

file(GLOB _plugin_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*)
foreach(_dir ${_plugin_dirs})
  if(IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/CMakeLists.txt)
    get_filename_component(_slug ${_dir} NAME)
    if(_factory_plugins AND NOT _slug IN_LIST _factory_plugins)
      continue()
    endif()
    add_subdirectory(${_dir})
  endif()
endforeach()
