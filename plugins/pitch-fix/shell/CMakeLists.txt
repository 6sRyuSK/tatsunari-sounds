# pitch-fix CLAP shell — the SHIPPING build (clap-first from birth).
#
# Assembled from the root CMakeLists whenever pitch-fix is configured (any
# plugin with a shell/CMakeLists.txt joins the clap-first assembly); it produces
# the CLAP + wrapper VST3 (+ AUv2 on Apple). The GUI (FACTORY_PF_CLAP_GUI, ON by
# default) embeds the Visage PfEditor. There is NO JUCE target anywhere in this
# plugin — the main CMakeLists.txt registers only the headless tests.

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml PF_CLAP_VERSION)
message(STATUS "pitch-fix CLAP shell: version ${PF_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the Pf Policy + the shared factory/entry glue
# (ClapEntry.cpp, via FACTORY_CLAP_ENTRY). The exported clap_entry lives in the
# shared ENTRY_SOURCE (shell/src/FactoryClapEntryPoint.cpp), which
# factory_clap_plugin() supplies by default — no per-plugin entry TU any more.
add_library(pitch-fix-impl STATIC ClapEntry.cpp)

target_include_directories(pitch-fix-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # PfClapEditor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..)      # PfCore.h / PfParams.h / PfPresets.h

target_link_libraries(pitch-fix-impl
  PUBLIC
    clap
    clap-wrapper-extensions
    factory_shell
    factory_core
    factory_params
    factory_presets)

target_compile_features(pitch-fix-impl PRIVATE cxx_std_20)
target_compile_definitions(pitch-fix-impl PRIVATE
  PF_CLAP_VERSION="${PF_CLAP_VERSION}")
set_target_properties(pitch-fix-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- GUI: embed the Visage Pf editor into the clap.gui extension --------------
# ON by default (the shipping build carries the editor); set OFF for a pure
# headless shell (validators / bring-up). This is the ONLY switch that pulls
# native Visage into the pitch-fix build.
option(FACTORY_PF_CLAP_GUI
  "Embed the Visage Pf editor into the pitch-fix CLAP (clap.gui); pulls native visage" ON)

if(FACTORY_PF_CLAP_GUI)
  # The shared Visage design-system library (native config) — added once per
  # build tree; another clap-first plugin's shell may already have added it.
  if(NOT TARGET factory_ui_visage)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ui/visage
                     ${CMAKE_BINARY_DIR}/factory_ui_visage)
  endif()

  set(PF_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

  # The Visage-backed editor host (the single visage TU) + the JUCE-free Pf
  # editor sources.
  target_sources(pitch-fix-impl PRIVATE
    PfClapEditor.cpp
    ${PF_UI_DIR}/PfEditor.cpp)

  target_include_directories(pitch-fix-impl PRIVATE ${PF_UI_DIR})

  # factory_ui_visage brings visage + the params/core includes transitively.
  target_link_libraries(pitch-fix-impl PUBLIC factory_ui_visage)

  target_compile_definitions(pitch-fix-impl PRIVATE FACTORY_PF_CLAP_GUI=1)

  message(STATUS "pitch-fix CLAP shell: GUI ON (Visage editor embedded)")
else()
  message(STATUS "pitch-fix CLAP shell: GUI OFF (headless shell)")
endif()

# Assemble CLAP + VST3 (+ AUv2 on Apple) with the house identifiers.
#   clap id / bundle id : jp.tatsunari-sounds.pitch-fix
#   AUv2 subtype        : Pfix
# Generated targets: pitch-fix_clap / _vst3 / _all.
factory_clap_plugin(pitch-fix
  IMPL_TARGET        pitch-fix-impl
  OUTPUT_NAME        "Pitch TatFixer"
  VERSION            ${PF_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  Pfix)
