# tools/vst3-probe - DEV-ONLY minimal VST3 host harness (issue #89).
#
# This is a STANDALONE project. It is deliberately NOT added by the root
# CMakeLists (which globs plugins/*/CMakeLists.txt, not tools/), so it never
# affects plugin builds, CI, or release artifacts. Build it on its own:
#
#   cmake -S tools/vst3-probe -B build-probe ^
#         -DVST3_SDK_DIR="<repo>/build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/VST3_SDK"
#   cmake --build build-probe --config Release
#
# VST3_SDK_DIR must point at a directory containing "pluginterfaces/". The VST3
# SDK ships inside the JUCE checkout that the main build fetches via FetchContent,
# so run the top-level `cmake -B build` once first (or pass any other VST3 SDK).
#
# Windows-only (LUNA is Windows); the harness uses Win32 + SEH.

cmake_minimum_required(VERSION 3.22)
project(vst3_probe LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "vst3-probe targets Windows only (issue #89 is Windows/LUNA).")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default the SDK location to the JUCE FetchContent checkout under the repo build dir.
set(_default_sdk "${CMAKE_CURRENT_LIST_DIR}/../../build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/VST3_SDK")
set(VST3_SDK_DIR "${_default_sdk}" CACHE PATH "Path to a VST3 SDK dir containing pluginterfaces/")

add_executable(vst3_probe vst3_probe.cpp)
target_include_directories(vst3_probe PRIVATE "${VST3_SDK_DIR}")

# /EHa: deliver C++ exceptions as SEH too, so a single __except can tell a
# C++ exception escaping the COM boundary from a hard fault.
target_compile_options(vst3_probe PRIVATE /EHa /W4)
target_compile_definitions(vst3_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
