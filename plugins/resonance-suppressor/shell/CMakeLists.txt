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
