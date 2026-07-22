# Idempotently apply one or more patches to the pinned visage checkout, used as the
# visage FetchContent PATCH_COMMAND. Invoked as:
#   cmake -DPATCH_FILES=<abs;abs;...> -P apply-visage-patches.cmake
# with the working directory set to the visage source tree (FetchContent runs
# PATCH_COMMAND in <SOURCE_DIR>).
#
# FetchContent/ExternalProject run PATCH_COMMAND directly (NOT through a shell), so
# shell operators like `||` cannot express "apply once". This script provides that
# idempotency in pure CMake, per patch: `git apply --reverse --check` succeeds ONLY
# when a patch is already present, in which case we skip it; otherwise we apply it.
# That makes a re-configure (which can re-run the patch step on an already-populated
# checkout) a no-op instead of a hard error. See ui/visage/CMakeLists.txt for why the
# patches exist; the pin hash itself is never changed.

if(NOT DEFINED PATCH_FILES)
  message(FATAL_ERROR "apply-visage-patches: PATCH_FILES not set")
endif()

foreach(patch IN LISTS PATCH_FILES)
  execute_process(
    COMMAND git apply --reverse --check "${patch}"
    RESULT_VARIABLE already_applied
    OUTPUT_QUIET ERROR_QUIET)

  if(already_applied EQUAL 0)
    message(STATUS "visage patch already applied, skipping: ${patch}")
    continue()
  endif()

  execute_process(
    COMMAND git apply --ignore-whitespace "${patch}"
    RESULT_VARIABLE apply_result)

  if(NOT apply_result EQUAL 0)
    message(FATAL_ERROR
      "visage patch failed (result ${apply_result}): ${patch}. The pin may have moved "
      "— re-evaluate ui/visage/patches/ against the new visage.")
  endif()

  message(STATUS "visage patch applied: ${patch}")
endforeach()
