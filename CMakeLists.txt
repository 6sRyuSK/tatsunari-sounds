cmake_minimum_required(VERSION 3.22)   # JUCE 8 requires CMake >= 3.22

project(PluginFactory VERSION 0.1.0 LANGUAGES C CXX)

# Objective-C/C++ must be enabled HERE — before the (conditional) JUCE fetch and
# before any subdirectory. Root cause (3.0.0 cutover, mac): with OBJCXX never
# enabled, CMake compiles .mm via the CXX rules and clang picks ObjC++ by
# extension — that's how the JUCE plugins' .mm modules built for months. But
# clap-wrapper (fetched for every clap-first build) enable_language()s
# OBJC/OBJCXX inside its own subdirectory, which flips the GLOBAL extension
# mapping: every .mm in the project now maps to the OBJCXX language, while
# directories processed before that point (anything carrying JUCE's .mm module
# unities — today the RS/dynamic-eq oracle test apps) never received the OBJCXX
# compile rules → the mac generate step dies with "Missing variable:
# CMAKE_OBJCXX_COMPILE_OBJECT" for ~every JUCE .mm. Enabling at the true root
# gives every directory the rules regardless of processing order.
if(APPLE)
  enable_language(OBJC)
  enable_language(OBJCXX)
endif()

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

# --- Which plugins are configured (decided BEFORE any dependency fetch) ------
# FACTORY_PLUGINS narrows the build to a subset of slugs (comma- or
# semicolon-separated). Empty (the default) configures everything. CI passes
# the plugins a PR touches so unaffected binaries are not rebuilt; anything
# that could affect all plugins (core/ ui/ cmake/ this file) keeps it empty.
#
# Archived plugins live under archive/plugins/<slug> (same layout) and are NOT
# configured by default — they are excluded from CI and releases. Pass
# -DFACTORY_INCLUDE_ARCHIVED=ON to add them to the configure set (local builds
# only); FACTORY_PLUGINS narrowing applies to archived slugs the same way.
#
# This runs BEFORE the JUCE fetch below on purpose: which heavyweight SDKs the
# configure pulls has to track the configured SET, not a global flag. The loop
# classifies each configured plugin twice —
#   * carries shell/CMakeLists.txt -> clap-first, joins the make_clapfirst
#     assembly at the bottom of this file (that is what fetches CLAP/VST3/
#     clap-wrapper + Visage);
#   * declares a juce_add_* target -> needs JUCE (the `^\s*juce_add_` match is
#     line-anchored so the "there is deliberately NO juce_add_plugin here"
#     comments in the active plugins do not count).
option(FACTORY_INCLUDE_ARCHIVED "Also configure archived plugins from archive/plugins/ (local use only; never CI/release)" OFF)
set(FACTORY_PLUGINS "" CACHE STRING "Plugin slugs to configure, comma/semicolon separated (empty = all)")
string(REPLACE "," ";" _factory_plugins "${FACTORY_PLUGINS}")

# 脱JUCE: no plugin SHIPS a JUCE binary any more. The only JUCE consumers left
# in the active tree are resonance-suppressor's and dynamic-eq's byte-equivalence
# / preset oracle console apps, which compile their frozen JUCE AudioProcessor as
# the oracle for the framework-free core. Turning this OFF skips those targets —
# and with them the whole JUCE FetchContent — for a configure that only needs the
# shipping (CLAP) path. ci.yml leaves it ON (that is where CTest runs the
# oracles); clap.yml's Linux legs pass OFF, since they only build the CLAP
# assembly and never ran the oracles anyway.
option(FACTORY_JUCE_ORACLES "Build the JUCE-linked equivalence/preset oracle tests (OFF also skips the JUCE fetch)" ON)

set(_clap_shell_dirs "")
set(_juce_slugs "")
set(_archived_slugs "")
set(_configured_plugin_dirs "")
file(GLOB _plugin_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/plugins/*)
if(FACTORY_INCLUDE_ARCHIVED)
  file(GLOB _archived_dirs LIST_DIRECTORIES true ${CMAKE_CURRENT_SOURCE_DIR}/archive/plugins/*)
  list(APPEND _plugin_dirs ${_archived_dirs})
endif()
foreach(_dir ${_plugin_dirs})
  if(IS_DIRECTORY ${_dir} AND EXISTS ${_dir}/CMakeLists.txt)
    get_filename_component(_slug ${_dir} NAME)
    if(_factory_plugins AND NOT _slug IN_LIST _factory_plugins)
      continue()
    endif()
    list(APPEND _configured_plugin_dirs ${_dir})
    if(_dir MATCHES "/archive/plugins/")
      list(APPEND _archived_slugs ${_slug})
    endif()
    if(EXISTS ${_dir}/shell/CMakeLists.txt)
      # A clap-first plugin: its shipping binaries come from its shell/, which
      # is assembled at the bottom of this file AFTER the generic shell library.
      list(APPEND _clap_shell_dirs ${_dir}/shell)
    endif()
    file(STRINGS ${_dir}/CMakeLists.txt _juce_decls REGEX "^[ \t]*juce_add_")
    if(_juce_decls)
      list(APPEND _juce_slugs ${_slug})
    endif()
  endif()
endforeach()

# An archived plugin IS a JUCE plugin (juce_add_plugin, ungated) — it has no
# clap-first path to fall back on, so the opt-out combination cannot produce
# anything. Fail at configure time instead of at the first missing juce:: target.
# Keyed on what is actually IN the set, so narrowing FACTORY_PLUGINS to a
# clap-only slug while FACTORY_INCLUDE_ARCHIVED is on stays legal.
if(_archived_slugs AND NOT FACTORY_JUCE_ORACLES)
  message(FATAL_ERROR
    "FACTORY_JUCE_ORACLES=OFF cannot configure the archived plugins in this set "
    "(${_archived_slugs}): they are JUCE plugins and need the JUCE fetch.")
endif()

# --- JUCE (pinned; fetched ONLY when a configured plugin declares a JUCE target)
# Post-脱JUCE this is the oracle-test dependency, not a shipping one: a configure
# whose plugin set is clap-only (e.g. -DFACTORY_PLUGINS=pitch-fix) or that passes
# -DFACTORY_JUCE_ORACLES=OFF never clones JUCE and never builds juceaide.
if(_juce_slugs AND FACTORY_JUCE_ORACLES)
  message(STATUS "JUCE 8.0.13 fetch: required by the oracle tests of ${_juce_slugs}")
  include(FetchContent)
  FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.13
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(JUCE)
elseif(_juce_slugs)
  message(STATUS "JUCE fetch skipped (FACTORY_JUCE_ORACLES=OFF); oracle tests of ${_juce_slugs} not configured")
else()
  message(STATUS "JUCE fetch skipped: no configured plugin declares a JUCE target")
endif()

# --- Shared DSP core (header-only for now; plugins compose from this) ---
add_library(factory_core INTERFACE)
target_include_directories(factory_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
target_compile_features(factory_core INTERFACE cxx_std_20)

# --- Shared JUCE design system (header-only; the factory's look & feel) ---
# 脱JUCE: nothing that SHIPS links this any more — the shipping editors are built
# from the compiled factory_ui_visage (ui/visage/). It survives as the look & feel
# of the RS/dynamic-eq oracle apps, so it is only ever consumed behind
# FACTORY_JUCE_ORACLES. Defined unconditionally (an INTERFACE library with no
# JUCE link is free) so a JUCE-less configure still resolves the name.
add_library(factory_ui INTERFACE)
target_include_directories(factory_ui INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ui/include)
target_compile_features(factory_ui INTERFACE cxx_std_20)

# --- Shared factory-preset model (header-only) ---
# PresetBank.h is JUCE-independent; ProgramAdapter.h rides the JUCE program API.
add_library(factory_presets INTERFACE)
target_include_directories(factory_presets INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/presets/include)
target_compile_features(factory_presets INTERFACE cxx_std_20)

# --- Shared parameter model (header-only) ---
# ParamDesc/Range/Text/ParamStore/UndoStack are JUCE-free; only juce/ApvtsAdapter.h
# rides the JUCE parameter API (same split as factory_presets).
add_library(factory_params INTERFACE)
target_include_directories(factory_params INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/params/include)
target_compile_features(factory_params INTERFACE cxx_std_20)

enable_testing()

# --- Shared update-check library (badge side; Visage-free / JUCE-free) ---
# Linked by clap GUI shells + its own headless CTest. Does not pull Visage.
add_subdirectory(update)

# --- Direct per-primitive tests for the shared factory_core DSP primitives ---
# (links only factory_core, headless; one CTest case per standard sample rate).
add_subdirectory(core/tests)

# --- Unit tests for the shared factory_params model ---
# (links ONLY factory_params, headless — proving the headers are JUCE-free; a
# single CTest case, nothing here is sample-rate dependent).
add_subdirectory(params/tests)

# --- Unit tests for the shared factory_presets model successors ---
# (links ONLY factory_presets + factory_params, headless — proving the new
# StateCodec/PresetSession/UserPresetStoreFs headers are JUCE-free; a single CTest
# case, nothing here is sample-rate dependent).
add_subdirectory(presets/tests)

# --- Configure every plugin selected above (classification already done) -----
foreach(_dir ${_configured_plugin_dirs})
  add_subdirectory(${_dir})
endforeach()

# --- CLAP-FIRST SHIPPING BUILDS (every active plugin) ------------------------
# Each active plugin (resonance-suppressor, pitch-fix, dynamic-eq) carries a
# shell/CMakeLists.txt whose make_clapfirst shell produces the CLAP + wrapper
# VST3 (+ AUv2 on Apple, with the Visage editor) — there is no juce_add_plugin
# for these plugins (their main CMakeLists holds only headless tests / test
# oracles). The generic shell + pinned CLAP/VST3/clap-wrapper SDKs are assembled
# whenever AT LEAST ONE clap-first plugin is in the configured set
# (_clap_shell_dirs), so an archive-only configure fetches nothing CLAP-related —
# the SDK fetch tracks the configured set, not a global flag (same rule as the
# JUCE fetch above).
#
# FACTORY_RS_CLAP is retained as a harmless no-op so a stray -DFACTORY_RS_CLAP=ON
# on a configure line (older scripts) neither errors nor changes anything.
option(FACTORY_RS_CLAP "Legacy no-op: RS is clap-first unconditionally now" OFF)
if(_clap_shell_dirs)
  # The impl + clap-wrapper's own static libs (base-sdk-vst3, the per-target VST3
  # wrapper glue) are linked into MODULE (shared) libraries, so they must be PIC.
  # Set it here — the root scope — so BOTH add_subdirectory()s below (and the
  # clap-wrapper targets they create) inherit it; the header-only INTERFACE libs +
  # the JUCE oracle test apps above are already defined and unaffected.
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)

  # Pin the MSVC C runtime for the WHOLE CLAP build tree, at the root scope, so
  # every target shares one CRT. clap-wrapper's own CMakeLists defaults
  # CMAKE_MSVC_RUNTIME_LIBRARY to the STATIC runtime (/MT) when it is undefined,
  # but our make_clapfirst targets + visage/bgfx use CMake's default DYNAMIC
  # runtime (/MD) — so the wrapper-VST3 link fails on Windows with LNK2038
  # ('RuntimeLibrary' mismatch: MT_StaticRelease vs MD_DynamicRelease) plus a
  # swarm of LNK2005 duplicate-symbol errors. clap-wrapper honours the variable
  # when it is already DEFINED, so defining it here (cached, before the
  # add_subdirectory()s that populate clap-wrapper) makes the entire tree /MD —
  # matching the JUCE oracle apps (also /MD), so every factory binary shares the
  # same CRT and the same redist assumption. (ci.yml builds every plugin
  # clap-first on macOS + Windows, so this Windows link path is gated on the real
  # shipping runners.) Left overridable — pass
  # -DCMAKE_MSVC_RUNTIME_LIBRARY=... (e.g. the static variant for a redist-free,
  # self-contained plugin) and this respects it, exactly like clap-wrapper does.
  if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    cmake_policy(SET CMP0091 NEW)  # runtime library via MSVC_RUNTIME_LIBRARY abstraction
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
        CACHE STRING "Dynamic MSVC CRT for the coexistence CLAP build (match JUCE)")
  endif()

  add_subdirectory(shell)
  foreach(_shell_dir ${_clap_shell_dirs})
    add_subdirectory(${_shell_dir})
  endforeach()
endif()
