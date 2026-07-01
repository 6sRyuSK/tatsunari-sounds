# cmake/NamCore.cmake — builds NeuralAmpModelerCore (Steven Atkinson, MIT) into an
# OBJECT library `nam_core` for the NAM Player plugin. This is the factory's first
# external dependency beyond JUCE; only plugins/nam-player links it.
#
# Integration notes (learned the hard way, keep them):
#  * NeuralAmpModelerCore exposes no consumable library target, and its own CMake
#    builds CLI tools that need the (empty) AudioDSPTools submodule. So we populate
#    the sources only (SOURCE_SUBDIR points at a folder with no CMakeLists so
#    FetchContent skips add_subdirectory) and compile NAM/*.cpp ourselves.
#  * nlohmann/json is vendored at Dependencies/nlohmann/json.hpp; sources include it
#    as "json.hpp", so that folder must be on the include path.
#  * Eigen is a submodule pointer (empty under FetchContent). NAM uses
#    Eigen::placeholders::lastN, which the 3.4.0 release does NOT provide, so we pin
#    the exact commit NAM's submodule points at.
#  * `nam_core` is an OBJECT library, not STATIC: the architectures self-register via
#    static initializers (e.g. `static ConfigParserHelper _register_WaveNet(...)` in
#    NAM/wavenet/model.cpp). A static archive drops those unreferenced objects,
#    giving "No config parser registered for architecture: WaveNet" at load. An
#    OBJECT library links every object into the consumer, so registration survives.

include_guard(GLOBAL)
include(FetchContent)

# Eigen — header-only; pinned to the exact commit NeuralAmpModelerCore v0.5.4 uses.
FetchContent_Declare(Eigen3
  GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
  GIT_TAG        bc3b39870ecb690a623a3f49149a358b95c5781d)
set(EIGEN_BUILD_DOC       OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_TESTING   OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
set(EIGEN_BUILD_PKGCONFIG OFF CACHE BOOL "" FORCE)

# NeuralAmpModelerCore — populate only (SOURCE_SUBDIR NAM has no CMakeLists.txt).
FetchContent_Declare(nam
  GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
  GIT_TAG        v0.5.4
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  NAM)

FetchContent_MakeAvailable(Eigen3 nam)

file(GLOB NAM_SOURCES CONFIGURE_DEPENDS
  ${nam_SOURCE_DIR}/NAM/*.cpp
  ${nam_SOURCE_DIR}/NAM/wavenet/*.cpp)

add_library(nam_core OBJECT ${NAM_SOURCES})
target_compile_features(nam_core PUBLIC cxx_std_20)
# SYSTEM includes so Eigen/NAM/json warnings never collide with the plugin's
# juce_recommended_warning_flags.
target_include_directories(nam_core SYSTEM PUBLIC
  ${nam_SOURCE_DIR}
  ${nam_SOURCE_DIR}/NAM
  ${nam_SOURCE_DIR}/Dependencies/nlohmann
  ${eigen3_SOURCE_DIR})
target_compile_definitions(nam_core PUBLIC NAM_SAMPLE_FLOAT)   # float audio path
if(MSVC)
  target_compile_definitions(nam_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
  target_compile_options(nam_core PRIVATE /bigobj /EHsc)        # Eigen/wavenet TUs are large
endif()
