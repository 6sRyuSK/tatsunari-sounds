# factory_ui_visage — the shared Visage design-system library (Phase P2a).
#
# This directory is add_subdirectory()-able from any parent project (tools/ui-dev
# now; the root later). It is intentionally NOT wired into the repository root
# CMakeLists yet — the JUCE plugin builds keep using ui/include/factory_ui/.
#
# It owns the visage dependency (pinned + patched for the sandbox) so consumers
# only have to `add_subdirectory(ui/visage)` and link `factory_ui_visage`.

cmake_minimum_required(VERSION 3.17) # visage requires >= 3.17

include(FetchContent)

set(FACTORY_VISAGE_GIT_TAG "20de59464243447816d142e9d38e9723d068f755" CACHE STRING
    "Pinned visage commit (S1 spike: HEAD of main, 2026-06-15)")

set(FACTORY_FREETYPE_MIRROR_DIR "" CACHE PATH
    "Optional local FreeType checkout (GitHub mirror, tag VER-2-14-1) used to \
satisfy visage's FetchContent when gitlab.freedesktop.org is blocked by the \
egress proxy. Empty (default) = let visage fetch FreeType normally.")

# --- Obstacle #1 (S1 spike): FreeType source override -------------------------
# visage pulls FreeType from gitlab.freedesktop.org via FetchContent; that host
# is blocked behind some egress proxies (403 CONNECT). When a mirror dir is
# supplied, hand it to CMake's own FetchContent source-dir override mechanism
# (no repo edit, no proxy bypass, no TLS disable).
if(FACTORY_FREETYPE_MIRROR_DIR)
  set(FETCHCONTENT_SOURCE_DIR_FREETYPE "${FACTORY_FREETYPE_MIRROR_DIR}" CACHE PATH "" FORCE)
endif()

# --- Obstacle #2 (S1 spike): legacy EMSCRIPTEN macro for bgfx/posh -------------
# Under Emscripten, bgfx's vendored nvtt POSH header gates its CPU/endian
# detection on the bare `EMSCRIPTEN` macro, which modern emcc no longer defines
# (only `__EMSCRIPTEN__`). Restore it here, BEFORE visage is populated, so it
# applies to visage's own translation units. Consumers never have to know.
if(EMSCRIPTEN)
  add_compile_definitions(EMSCRIPTEN=1)
endif()

# --- Obstacle #3: visage patches for the RS clap-first Retina editor -----------
# Applied via PATCH_COMMAND against the pinned checkout (the pin hash is NEVER changed).
# apply-visage-patches.cmake applies each idempotently (a re-configure that re-runs the
# patch step on an already-populated checkout is a no-op, not an error) — see that script
# for why a plain `git apply` / shell `||` cannot be used here. RE-EVALUATE these patches
# whenever FACTORY_VISAGE_GIT_TAG is bumped (upstream lines may move, or the fixes may land
# upstream). See ui/visage/patches/ for the diffs + rationale.
#
# 0001 (macOS backing-scale vs layout-zoom split): the pinned visage reuses
#   Window::dpiScale() for BOTH the frame-tree layout scale AND the macOS physical-pixel
#   density (MTKView contentsScale / drawable size, mouse point<->pixel conversion). The RS
#   editor drives the window dpi as a UNIFORM ZOOM (dpi = physicalHeight/747) so the 1069x747
#   design fills any window; on a Retina Mac that zoom is NOT the backing factor (2.0), so the
#   shared value left the drawable undersized (black margin) and clicks offset (M1 Air/Logic).
#   Adds a SEPARATE WindowMac backingScale() for every points<->pixels conversion and leaves
#   dpiScale() as the pure layout scale. Also disables MTKView autoResizeDrawable so bgfx is
#   the SOLE writer of the drawable size (no two-writer magenta overhang). macOS-only files —
#   Windows / X11 / Emscripten unaffected.
# 0002 (clear the window framebuffer): visage never bgfx::setViewClear's, so any pixel of the
#   on-screen framebuffer not covered by drawn geometry is uninitialised (magenta). This clears
#   the WINDOW layer to the warm-white theme background as a safety net (normally invisible).
#   Cross-platform (visage_graphics), harmless where the editor already covers the surface.
# 0003 (embedded-view backing scale + base accessor): visage's WindowMac::resetBackingScale
#   only tracked the OS backingScaleFactor when window_handle_ was set — null for a plugin
#   SUBVIEW — so an embedded editor's backingScale() stayed frozen at the main-screen value
#   captured at creation. When Logic hosts the AU on a display of a different scale, the subview
#   frame (client / backingScale points) is mis-sized and the right + bottom clip. This reads the
#   scale from the actual host window ([view_ window]) for embedded views, and adds a virtual
#   Window::backingScale() (default dpiScale()) so the shell can read it without a platform cast.
# 0004 (host content size for embedded views): adds a virtual Window::hostContentSizePx()
#   (default = client size) that WindowMac overrides to return the PARENT view's real bounds
#   in physical px. Logic can make its content area SMALLER than the size it requested via
#   set_size (its window chrome imposes a minimum — measured ~458pt when it asked for the
#   471pt minimum), so the shell renders to this real area instead of the requested size,
#   which otherwise overflows the container and clips the right + bottom.
#
# The patch step is idempotent by RESET (apply-visage-patches.cmake hard-resets the pinned
# checkout, then re-applies): the patches touch overlapping regions of the same files, which
# a reverse-check-to-skip scheme cannot handle. Re-evaluate ALL patches when the pin bumps.
# The patch step receives the patches DIRECTORY (the script globs NNNN-*.patch,
# sorted) — never a `;`-joined list: ExternalProject's command serialisation splits
# an embedded list separator into a stray positional argument that `cmake -P`
# silently ignores, dropping every patch after the first (bit us in the field:
# 0001 applied, 0002 silently missing).
FetchContent_Declare(visage
  GIT_REPOSITORY https://github.com/VitalAudio/visage.git
  GIT_TAG ${FACTORY_VISAGE_GIT_TAG}
  PATCH_COMMAND ${CMAKE_COMMAND}
                "-DPATCH_DIR=${CMAKE_CURRENT_SOURCE_DIR}/patches"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/apply-visage-patches.cmake
)
# As a subproject, visage sets VISAGE_BUILD_EXAMPLES / _TESTS OFF automatically
# and (under Emscripten) skips the CLAP example + SDK fetch.
FetchContent_MakeAvailable(visage)

# --- embedded typeface (PROVISIONAL Quicksand, SIL OFL 1.1) -------------------
# Uses visage's own build-time resource embedder (available once visage is
# populated). Namespace factory_ui_visage::fonts, header embedded/<name>.h.
# See ui/visage/fonts/OFL.txt for the licence; swapping the face is a one-place
# change (this glob + the two symbol names in src/Fonts.cpp — see Fonts.h).
file(GLOB FACTORY_UI_VISAGE_FONT_FILES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/fonts/*.ttf")
add_embedded_resources(factory_ui_visage_fonts "factory_ui_visage_fonts.h"
                       "factory_ui_visage::fonts" "${FACTORY_UI_VISAGE_FONT_FILES}")

# --- the design-system library ------------------------------------------------
add_library(factory_ui_visage STATIC
  src/Theme.cpp
  src/Fonts.cpp
  src/Chrome.cpp
  src/Knob.cpp
  src/PillToggle.cpp
  # --- Phase P2b widget set ---
  src/Dropdown.cpp
  src/PresetSelectorView.cpp
  src/Segmented.cpp
  src/IconButton.cpp
  src/ValueSetting.cpp
  src/ValueEntry.cpp
  src/LinkSlider.cpp
  src/SpectrumModel.cpp
  src/SpectrumView.cpp
)

target_compile_features(factory_ui_visage PUBLIC cxx_std_17)

# Our own public headers, plus the shared JUCE-free params/core headers our public
# headers (Knob.h / PillToggle.h) include. Exposed PUBLIC so consumers inherit them.
target_include_directories(factory_ui_visage PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../params/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
)

target_link_libraries(factory_ui_visage PUBLIC
  visage
  VisageWidgets   # ValueEntry uses visage::TextEditor (visage_widgets); make the
                  # link explicit rather than relying on the visage meta-target's
                  # PRIVATE deps propagating through the static archive.
  factory_ui_visage_fonts
)

# --- host-side theme unit test (native only) ----------------------------------
# The Theme model + JSON parser are visage-free and JUCE-free, so they can be
# checked with a plain host compiler. Guarded on NOT EMSCRIPTEN so it never joins
# the wasm gallery build; only Theme.cpp is compiled (no visage dependency).
if(NOT EMSCRIPTEN)
  add_executable(factory_ui_visage_theme_test
    tests/theme_roundtrip_test.cpp
    src/Theme.cpp
  )
  target_include_directories(factory_ui_visage_theme_test PRIVATE include)
  target_compile_features(factory_ui_visage_theme_test PRIVATE cxx_std_17)

  # CTest case (rate-independent, one case). The shipped default document is
  # passed explicitly: the exe's built-in "../theme/factory-default.json"
  # fallback is relative to a manual-run cwd, not the ctest working directory.
  add_test(NAME factory_ui_visage_theme
           COMMAND factory_ui_visage_theme_test
                   ${CMAKE_CURRENT_SOURCE_DIR}/theme/factory-default.json)

  # SpectrumModel is visage-free AND JUCE-free (pure maths + factory_core::FFT),
  # so its spec/oracle check compiles with a plain host compiler like the theme
  # test — only SpectrumModel.cpp is built (no visage), plus the core headers.
  add_executable(factory_ui_visage_spectrum_test
    tests/spectrum_model_test.cpp
    src/SpectrumModel.cpp
  )
  target_include_directories(factory_ui_visage_spectrum_test PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../core/include
  )
  target_compile_features(factory_ui_visage_spectrum_test PRIVATE cxx_std_17)

  # Full standard sample-rate matrix (sampleRatesFromArgs), one CTest case per
  # rate — same shape as a plugin dsp_test (fftOrderForSampleRate is under test).
  foreach(_fs 44100 48000 88200 96000 176400 192000)
    add_test(NAME factory_ui_visage_spectrum_${_fs}
             COMMAND factory_ui_visage_spectrum_test ${_fs})
  endforeach()

  # ValueText (the visage-free half of the direct value-entry flow) is headers
  # only — compiling it here with a plain host compiler also proves it stays
  # visage-free and JUCE-free. Rate-independent, one case.
  add_executable(factory_ui_visage_value_text_test tests/value_text_test.cpp)
  target_include_directories(factory_ui_visage_value_text_test PRIVATE
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../../params/include
  )
  target_compile_features(factory_ui_visage_value_text_test PRIVATE cxx_std_17)
  add_test(NAME factory_ui_visage_value_text COMMAND factory_ui_visage_value_text_test)
endif()
