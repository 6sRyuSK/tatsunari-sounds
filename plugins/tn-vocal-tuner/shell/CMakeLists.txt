# tn-vocal-tuner CLAP shell — the SHIPPING build (clap-first from birth).
#
# Assembled from the root CMakeLists whenever tn-vocal-tuner is configured (any
# plugin with a shell/CMakeLists.txt joins the clap-first assembly); it produces
# the CLAP + wrapper VST3 (+ AUv2 on Apple). The GUI (FACTORY_PF_CLAP_GUI, ON by
# default) embeds the Visage PfEditor. There is NO JUCE target anywhere in this
# plugin — the main CMakeLists.txt registers only the headless tests.

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml PF_CLAP_VERSION)
message(STATUS "tn-vocal-tuner CLAP shell: version ${PF_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the Pf Policy + the shared factory/entry glue
# (ClapEntry.cpp, via FACTORY_CLAP_ENTRY). The exported clap_entry lives in the
# shared ENTRY_SOURCE (shell/src/FactoryClapEntryPoint.cpp), which
# factory_clap_plugin() supplies by default — no per-plugin entry TU any more.
add_library(tn-vocal-tuner-impl STATIC ClapEntry.cpp)

target_include_directories(tn-vocal-tuner-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # PfClapEditor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..)      # PfCore.h / PfParams.h / PfPresets.h

target_link_libraries(tn-vocal-tuner-impl
  PUBLIC
    clap
    clap-wrapper-extensions
    factory_shell
    factory_core
    factory_params
    factory_presets)

target_compile_features(tn-vocal-tuner-impl PRIVATE cxx_std_20)
target_compile_definitions(tn-vocal-tuner-impl PRIVATE
  PF_CLAP_VERSION="${PF_CLAP_VERSION}")
set_target_properties(tn-vocal-tuner-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- GUI: embed the Visage Pf editor into the clap.gui extension --------------
# ON by default (the shipping build carries the editor); set OFF for a pure
# headless shell (validators / bring-up). This is the ONLY switch that pulls
# native Visage into the tn-vocal-tuner build.
option(FACTORY_PF_CLAP_GUI
  "Embed the Visage Pf editor into the tn-vocal-tuner CLAP (clap.gui); pulls native visage" ON)

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
  target_sources(tn-vocal-tuner-impl PRIVATE
    PfClapEditor.cpp
    ${PF_UI_DIR}/PfEditor.cpp)

  target_include_directories(tn-vocal-tuner-impl PRIVATE ${PF_UI_DIR})

  # factory_ui_visage_clap_host brings the shared Visage IClapEditor host base +
  # factory_ui_visage (visage + params/core includes) + factory_shell + clap.
  # factory_update backs the in-editor update badge (UpdateUiHost).
  target_link_libraries(tn-vocal-tuner-impl PUBLIC factory_ui_visage_clap_host factory_update)

  target_compile_definitions(tn-vocal-tuner-impl PRIVATE FACTORY_PF_CLAP_GUI=1)

  message(STATUS "tn-vocal-tuner CLAP shell: GUI ON (Visage editor embedded)")
else()
  message(STATUS "tn-vocal-tuner CLAP shell: GUI OFF (headless shell)")
endif()

# Assemble CLAP + VST3 (+ AUv2 on Apple) with the house identifiers.
#   clap id / bundle id : jp.tatsunari-sounds.tn-vocal-tuner
#   AUv2 subtype        : Tnvt (newly assigned for the tn-* product; §11.3 forbids
#                         reusing the retired product's subtype)
# Generated targets: tn-vocal-tuner_clap / _vst3 / _all.
factory_clap_plugin(tn-vocal-tuner
  IMPL_TARGET        tn-vocal-tuner-impl
  OUTPUT_NAME        "tn-vocal-tuner"
  VERSION            ${PF_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  Tnvt)
