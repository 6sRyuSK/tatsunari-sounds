# dynamic-eq CLAP shell — the SHIPPING build (脱JUCE cutover, 2.0.0).
#
# Assembled from the root CMakeLists whenever dynamic-eq is configured (any plugin with a
# shell/CMakeLists.txt joins the clap-first assembly); it produces the CLAP + wrapper VST3
# (+ AUv2 on Apple) that REPLACE the old JUCE VST3/AU. The GUI (FACTORY_DEQ_CLAP_GUI, ON by
# default) embeds the Visage DeqEditor. The plugin's main CMakeLists.txt keeps only the
# JUCE-linked test oracles (deqcore_equiv / preset), never a shipping juce_add_plugin.

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml DEQ_CLAP_VERSION)
message(STATUS "dynamic-eq CLAP shell: version ${DEQ_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the Deq Policy + the shared factory/entry glue (ClapEntry.cpp,
# via FACTORY_CLAP_ENTRY). The exported clap_entry lives in the shared ENTRY_SOURCE
# (shell/src/FactoryClapEntryPoint.cpp), which factory_clap_plugin() supplies by default.
add_library(dynamic-eq-impl STATIC ClapEntry.cpp)

target_include_directories(dynamic-eq-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # DeqClapEditor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..        # DeqCore.h / DeqParams.h / DeqStateMigration.h / Source/*
  ${CMAKE_CURRENT_SOURCE_DIR}/../ui)    # DeqEditor.h / DeqCurveView.h / DeqBandPanel.h / DeqModels.h

target_link_libraries(dynamic-eq-impl
  PUBLIC
    clap
    clap-wrapper-extensions
    factory_shell
    factory_core
    factory_params
    factory_presets)

target_compile_features(dynamic-eq-impl PRIVATE cxx_std_20)
target_compile_definitions(dynamic-eq-impl PRIVATE
  DEQ_CLAP_VERSION="${DEQ_CLAP_VERSION}")
set_target_properties(dynamic-eq-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- GUI: embed the Visage Deq editor into the clap.gui extension -------------
# ON by default (the shipping build carries the editor); set OFF for a pure headless shell
# (validators / bring-up). This is the ONLY switch that pulls native Visage into the build.
option(FACTORY_DEQ_CLAP_GUI
  "Embed the Visage Deq editor into the dynamic-eq CLAP (clap.gui); pulls native visage" ON)

if(FACTORY_DEQ_CLAP_GUI)
  # The shared Visage design-system library (native config) — added once per build tree;
  # another clap-first plugin's shell may already have added it.
  if(NOT TARGET factory_ui_visage)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ui/visage
                     ${CMAKE_BINARY_DIR}/factory_ui_visage)
  endif()

  set(DEQ_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

  # The Visage-backed editor host (the single visage TU) + the JUCE-free Deq editor sources.
  target_sources(dynamic-eq-impl PRIVATE
    DeqClapEditor.cpp
    ${DEQ_UI_DIR}/DeqEditor.cpp
    ${DEQ_UI_DIR}/DeqCurveView.cpp
    ${DEQ_UI_DIR}/DeqBandPanel.cpp)

  # factory_ui_visage_clap_host brings the shared Visage IClapEditor host base +
  # factory_ui_visage (visage + params/core includes) + factory_shell + clap.
  target_link_libraries(dynamic-eq-impl PUBLIC factory_ui_visage_clap_host)

  target_compile_definitions(dynamic-eq-impl PRIVATE FACTORY_DEQ_CLAP_GUI=1)

  message(STATUS "dynamic-eq CLAP shell: GUI ON (Visage editor embedded)")
else()
  message(STATUS "dynamic-eq CLAP shell: GUI OFF (headless shell)")
endif()

# Assemble CLAP + VST3 (+ AUv2 on Apple) with the house identifiers.
#   clap id / bundle id : jp.tatsunari-sounds.dynamic-eq
#   AUv2 subtype        : Dyeq (matches the JUCE PLUGIN_CODE for AU continuity)
# Generated targets: dynamic-eq_clap / _vst3 / _all.
factory_clap_plugin(dynamic-eq
  IMPL_TARGET        dynamic-eq-impl
  OUTPUT_NAME        "Dynamic TatEQ"
  VERSION            ${DEQ_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  Dyeq)
