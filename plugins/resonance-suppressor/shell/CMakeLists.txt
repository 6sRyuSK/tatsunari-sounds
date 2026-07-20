# resonance-suppressor CLAP shell (Phase P4 coexistence, chunk 3a).
#
# Added ONLY from the root under FACTORY_RS_CLAP — the plugin's main
# CMakeLists.txt (the shipping JUCE VST3/AU) is untouched and never references
# this. Produces the CLAP + wrapper-VST3 from RsCore + factory_shell, headless
# (no Visage/editor link; the clap.gui editor is chunk 3b).

# plugin.toml stays the single source of truth for the version (catalog == binary).
factory_read_version(${CMAKE_CURRENT_SOURCE_DIR}/../plugin.toml RS_CLAP_VERSION)
message(STATUS "resonance-suppressor CLAP shell: version ${RS_CLAP_VERSION} (from plugin.toml)")

# The impl static library: the RS Policy + factory + entry hooks (ClapEntry.cpp).
# The tiny per-format ENTRY_SOURCE (RsClapEntryPoint.cpp) is handed to
# make_clapfirst_plugins separately (it is recompiled once per format).
add_library(resonance-suppressor-impl STATIC ClapEntry.cpp)

target_include_directories(resonance-suppressor-impl PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}          # RsClapEntry.h
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

# --- GUI (chunk 3b): embed the Visage RS editor into the clap.gui extension ----
# ON by default whenever the CLAP shell is built (FACTORY_RS_CLAP); set OFF to get
# the pure-headless 3a shell back. This is the ONLY switch that pulls native Visage
# into the RS build — the shipping JUCE VST3/AU never reaches here (FACTORY_RS_CLAP
# is OFF for it), so it never sees Visage.
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
  # The shared Visage design-system library (native config), added once here.
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../ui/visage
                   ${CMAKE_BINARY_DIR}/factory_ui_visage)

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

  # factory_ui_visage brings visage + the params/core includes transitively.
  target_link_libraries(resonance-suppressor-impl PUBLIC factory_ui_visage)

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
  ENTRY_SOURCE       ${CMAKE_CURRENT_SOURCE_DIR}/RsClapEntryPoint.cpp
  OUTPUT_NAME        "Resonance TatSuppressor"
  VERSION            ${RS_CLAP_VERSION}
  AUV2_SUBTYPE_CODE  Rsup)
