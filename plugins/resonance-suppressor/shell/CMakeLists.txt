# resonance-suppressor CLAP shell — the SHIPPING build (3.0.0 cutover).
#
# Assembled from the root CMakeLists whenever RS is configured (_rs_configured);
# it produces the CLAP + wrapper VST3 (+ AUv2 on Apple) that REPLACE the old JUCE
# VST3/AU. The GUI (FACTORY_RS_CLAP_GUI, ON by default) embeds the Visage RsEditor.
# The plugin's main CMakeLists.txt keeps only the JUCE-linked test oracles
# (rscore_equiv / preset), never a shipping juce_add_plugin.

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml RS_CLAP_VERSION)
message(STATUS "resonance-suppressor CLAP shell: version ${RS_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the RS Policy + the shared factory/entry glue
# (ClapEntry.cpp, via FACTORY_CLAP_ENTRY). The exported clap_entry lives in the
# shared ENTRY_SOURCE (shell/src/FactoryClapEntryPoint.cpp), which
# factory_clap_plugin() supplies by default — no per-plugin entry TU any more.
add_library(resonance-suppressor-impl STATIC ClapEntry.cpp)

target_include_directories(resonance-suppressor-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # RsClapEditor.h
  ${CMAKE_CURRENT_SOURCE_DIR}/..)      # RsCore.h + Source/*.h (Params/FactoryPresets/DetailParam)

target_link_libraries(resonance-suppressor-impl
  PUBLIC
    clap
    clap-wrapper-extensions
    factory_shell
    factory_core
    factory_params
    factory_presets)

target_compile_features(resonance-suppressor-impl PRIVATE cxx_std_20)
target_compile_definitions(resonance-suppressor-impl PRIVATE
  RS_CLAP_VERSION="${RS_CLAP_VERSION}")
set_target_properties(resonance-suppressor-impl PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --- GUI: embed the Visage RS editor into the clap.gui extension --------------
# ON by default (the shipping build carries the editor); set OFF for a pure-headless
# shell (used by some validators / bring-up). This is the ONLY switch that pulls
# native Visage into the RS build; the JUCE test oracles (rscore_equiv / preset) use
# ui/include/factory_ui, never Visage.
#
# ui/visage owns the pinned + sandbox-patched visage dependency; the operator hands
# it the pinned checkout + bgfx source + FreeType mirror the same way tools/ui-dev
# does, via -DFETCHCONTENT_SOURCE_DIR_VISAGE / -DFETCHCONTENT_SOURCE_DIR_BGFX /
# -DFACTORY_FREETYPE_MIRROR_DIR on the configure line (CI on mac/Win lets it fetch).
# Native visage additionally needs the OpenGL dev libs (bgfx find_package(OpenGL));
# the mac/Win runners ship them, a headless Linux box installs mesa-common-dev +
# libgl-dev.
option(FACTORY_RS_CLAP_GUI
  "Embed the Visage RS editor into the RS CLAP (clap.gui); pulls native visage" ON)

if(FACTORY_RS_CLAP_GUI)
  # The shared Visage design-system library (native config) — added once per
  # build tree; another clap-first plugin's shell may already have added it.
  if(NOT TARGET factory_ui_visage)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ui/visage
                     ${CMAKE_BINARY_DIR}/factory_ui_visage)
  endif()

  set(RS_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../ui)

  # The Visage-backed editor host (the single visage TU) + the JUCE-free RS editor
  # sources (the P3 port) — compiled into the impl library, exactly the set the
  # tools/ui-dev rs-editor harness builds.
  target_sources(resonance-suppressor-impl PRIVATE
    RsClapEditor.cpp
    ${RS_UI_DIR}/RsEditor.cpp
    ${RS_UI_DIR}/RsSuppressionCurveView.cpp
    ${RS_UI_DIR}/RsNodePanel.cpp)

  target_include_directories(resonance-suppressor-impl PRIVATE ${RS_UI_DIR})

  # factory_ui_visage_clap_host brings the shared Visage IClapEditor host base +
  # factory_ui_visage (visage + params/core includes) + factory_shell + clap.
  target_link_libraries(resonance-suppressor-impl PUBLIC factory_ui_visage_clap_host)

  target_compile_definitions(resonance-suppressor-impl PRIVATE FACTORY_RS_CLAP_GUI=1)

  message(STATUS "resonance-suppressor CLAP shell: GUI ON (Visage editor embedded)")
else()
  message(STATUS "resonance-suppressor CLAP shell: GUI OFF (headless 3a shell)")
endif()

# Assemble CLAP + VST3 (+ AUv2 on Apple) with the house identifiers.
#   clap id / bundle id : jp.tatsunari-sounds.resonance-suppressor
#   AUv2 subtype        : Rsup (matches the JUCE PLUGIN_CODE)
# Generated targets: resonance-suppressor_clap / _vst3 / _all.
factory_clap_plugin(resonance-suppressor
  IMPL_TARGET        resonance-suppressor-impl
  OUTPUT_NAME        "Resonance TatSuppressor"
  VERSION            ${RS_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  Rsup)

# ---- CLAP shell headless test -------------------------------------------------
# Host-facing param OUTPUT events (GUI knob edit -> begin/value/end to the host),
# the 3.0.0 clean-break state path (v1 round-trips; a JUCE/v0 blob -> defaults; a
# bad blob rejected), and the real A/B (params + program index). Links ONLY
# factory_shell (+ its factory_params / factory_presets) — no JUCE, no Visage.
add_executable(resonance_suppressor_clap_shell_test
  ${CMAKE_CURRENT_SOURCE_DIR}/../tests/clap_shell_test.cpp)
target_link_libraries(resonance_suppressor_clap_shell_test PRIVATE factory_shell)
target_include_directories(resonance_suppressor_clap_shell_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..        # plugin root: "Source/..." and "ui/..."
  ${CMAKE_CURRENT_SOURCE_DIR}/../ui)    # "RsModels.h" included by ui/RsAbState.h
target_compile_features(resonance_suppressor_clap_shell_test PRIVATE cxx_std_20)
add_test(NAME resonance_suppressor_clap_shell COMMAND resonance_suppressor_clap_shell_test)
