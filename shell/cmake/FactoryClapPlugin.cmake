# FactoryClapPlugin.cmake — house wrapper around clap-wrapper's
# make_clapfirst_plugins, with the SDK dependency wiring centralised once (the
# same way the root CMakeLists pins the shipping audio framework at 8.0.13).
#
# Including this module (from shell/CMakeLists.txt) FetchContent-populates the
# pinned CLAP + VST3 + clap-wrapper SDKs — but ONLY the first time (guarded), and
# ONLY when the caller has opted into the CLAP build (this file is reached solely
# under FACTORY_RS_CLAP). A default `cmake -B build` never includes it, so it
# fetches nothing CLAP-related and the shipping build's gate is unaffected.
#
# Proxy-robust strategy proven by the S2 spike (docs/migration/s2-clap-first.md):
#   * clap + vst3sdk are populated by git (works through the egress proxy) and
#     handed to clap-wrapper via the `clap` target / VST3_SDK_ROOT.
#   * CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES stays OFF, so clap-wrapper never runs its
#     vendored CPM bootstrap (which downloads CPM.cmake from a GitHub release URL).

include_guard(GLOBAL)

include(FetchContent)

# --- Pins (recorded in docs/migration/s2-clap-first.md) ----------------------
set(FACTORY_CLAP_TAG           "1.2.10"                                     CACHE STRING "free-audio/clap tag")
set(FACTORY_CLAPWRAPPER_COMMIT "35f524b771ec09f54c164720bb90f271273b37d3"  CACHE STRING "free-audio/clap-wrapper commit (v0.15.1)")
set(FACTORY_VST3SDK_TAG        "v3.8.0_build_66"                           CACHE STRING "steinbergmedia/vst3sdk tag")

# The impl static lib is linked into MODULE libraries, so PIC is required.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Populate the SDKs + define make_clapfirst_plugins exactly once per configure.
if(NOT TARGET clap-wrapper-extensions)
  message(STATUS "factory_clap: fetching CLAP + VST3 + clap-wrapper SDKs (FACTORY_RS_CLAP=ON)")

  # 1. CLAP SDK — MakeAvailable creates the header-only `clap` INTERFACE target;
  #    clap-wrapper's guarantee_clap() then sees TARGET clap and aliases it.
  FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG        ${FACTORY_CLAP_TAG}
    GIT_SHALLOW    ON)
  FetchContent_MakeAvailable(clap)

  # 2. VST3 SDK — populate-only (clap-wrapper compiles base-sdk-vst3 itself);
  #    hand it the root via VST3_SDK_ROOT. Only the 4 submodules clap-wrapper needs.
  FetchContent_Declare(vst3sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
    GIT_TAG        ${FACTORY_VST3SDK_TAG}
    GIT_SUBMODULES base public.sdk pluginterfaces cmake
    GIT_SHALLOW    ON
    GIT_PROGRESS   ON)
  FetchContent_GetProperties(vst3sdk)
  if(NOT vst3sdk_POPULATED)
    FetchContent_Populate(vst3sdk)
  endif()
  set(VST3_SDK_ROOT "${vst3sdk_SOURCE_DIR}" CACHE PATH "VST3 SDK root" FORCE)
  message(STATUS "factory_clap: VST3_SDK_ROOT = ${VST3_SDK_ROOT}")

  # 2b. AudioUnitSDK (Apple only) — the AUv2 wrapper's guarantee_auv2sdk() needs
  #     AUDIOUNIT_SDK_ROOT when downloads are OFF (its only other paths are the
  #     CPM download we disable and a local-dir search that fails on CI). Same
  #     populate-only pattern as the VST3 SDK; pinned to the exact tag the pinned
  #     clap-wrapper would CPM-fetch itself (parity, see base_sdks.cmake). Only
  #     configured on APPLE — Linux/Windows never reach the AUv2 path.
  if(APPLE)
    set(FACTORY_AUV2SDK_TAG "AudioUnitSDK-1.1.0" CACHE STRING "apple/AudioUnitSDK tag")
    FetchContent_Declare(audiounitsdk
      GIT_REPOSITORY https://github.com/apple/AudioUnitSDK.git
      GIT_TAG        ${FACTORY_AUV2SDK_TAG}
      GIT_SHALLOW    ON)
    FetchContent_GetProperties(audiounitsdk)
    if(NOT audiounitsdk_POPULATED)
      FetchContent_Populate(audiounitsdk)
    endif()
    set(AUDIOUNIT_SDK_ROOT "${audiounitsdk_SOURCE_DIR}" CACHE PATH "AudioUnit SDK root" FORCE)
    message(STATUS "factory_clap: AUDIOUNIT_SDK_ROOT = ${AUDIOUNIT_SDK_ROOT}")
  endif()

  # 3. clap-wrapper — downloads OFF so its vendored CPM bootstrap never runs.
  set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES OFF CACHE BOOL "" FORCE)
  set(CLAP_WRAPPER_BUILD_TESTS           OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG        ${FACTORY_CLAPWRAPPER_COMMIT})
  FetchContent_MakeAvailable(clap-wrapper)
endif()

# factory_clap_plugin(<slug>
#     IMPL_TARGET   <target>          # static lib you defined (impl + entry hooks)
#     ENTRY_SOURCE  <cpp>             # the tiny per-format clap_entry TU
#     OUTPUT_NAME   "<Product Name>"  # e.g. "Resonance TatSuppressor"
#     VERSION       <x.y.z>           # from plugin.toml (factory_read_version)
#     AUV2_SUBTYPE_CODE <code>)       # 4-char AU subtype (manufacturer is fixed Ttsn)
#
# Assembles CLAP + VST3 (+ AUV2 on Apple) with the house identifiers:
#   bundle id  jp.tatsunari-sounds.<slug>
#   AUv2 mfr   Ttsn (matches the shipping build's PLUGIN_MANUFACTURER_CODE)
# Generated targets: <slug>_clap, <slug>_vst3, and the aggregate <slug>_all.
function(factory_clap_plugin slug)
  set(oneValueArgs IMPL_TARGET ENTRY_SOURCE OUTPUT_NAME VERSION AUV2_SUBTYPE_CODE)
  cmake_parse_arguments(FCP "" "${oneValueArgs}" "" ${ARGN})

  if(NOT FCP_IMPL_TARGET)
    message(FATAL_ERROR "factory_clap_plugin(${slug}): IMPL_TARGET is required")
  endif()
  if(NOT FCP_ENTRY_SOURCE)
    message(FATAL_ERROR "factory_clap_plugin(${slug}): ENTRY_SOURCE is required")
  endif()
  if(NOT FCP_OUTPUT_NAME)
    set(FCP_OUTPUT_NAME "${slug}")
  endif()
  if(NOT FCP_VERSION)
    set(FCP_VERSION "0.0.0")
  endif()

  # Formats: CLAP + VST3 everywhere; AUv2 only on Apple (make_clapfirst guards
  # AUv2/AUv3/AAX behind APPLE anyway, and AUv3 additionally needs -G Xcode).
  set(_formats CLAP VST3)
  if(APPLE)
    list(APPEND _formats AUV2)
  endif()

  make_clapfirst_plugins(
    TARGET_NAME        ${slug}
    IMPL_TARGET        ${FCP_IMPL_TARGET}

    OUTPUT_NAME        "${FCP_OUTPUT_NAME}"

    ENTRY_SOURCE       ${FCP_ENTRY_SOURCE}

    BUNDLE_IDENTIFIER  "jp.tatsunari-sounds.${slug}"
    BUNDLE_VERSION     ${FCP_VERSION}

    COPY_AFTER_BUILD   FALSE

    PLUGIN_FORMATS     ${_formats}

    # Windows VST3 must be the folder-form bundle for its validator to pass
    # (clap-wrapper example note; harmless on Linux/macOS).
    WINDOWS_FOLDER_VST3 TRUE

    AUV2_MANUFACTURER_NAME "Tatsunari Sounds"
    AUV2_MANUFACTURER_CODE Ttsn
    AUV2_SUBTYPE_CODE      ${FCP_AUV2_SUBTYPE_CODE}

    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${slug}_assets
  )
endfunction()
