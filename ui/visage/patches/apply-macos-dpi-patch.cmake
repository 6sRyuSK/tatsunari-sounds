# Idempotently apply a patch to the pinned visage checkout, used as the visage
# FetchContent PATCH_COMMAND. Invoked as:
#   cmake -DPATCH_FILE=<abs path> -P apply-macos-dpi-patch.cmake
# with the working directory set to the visage source tree (FetchContent runs
# PATCH_COMMAND in <SOURCE_DIR>).
#
# FetchContent/ExternalProject run PATCH_COMMAND directly (NOT through a shell), so
# shell operators like `||` cannot express "apply once". This script provides that
# idempotency in pure CMake: `git apply --reverse --check` succeeds ONLY when the
# patch is already present, in which case we skip; otherwise we apply it. That makes
# a re-configure (which can re-run the patch step on an already-populated checkout)
# a no-op instead of a hard error. See ui/visage/CMakeLists.txt for why the patch
# exists; the pin hash itself is never changed.

if(NOT DEFINED PATCH_FILE)
  message(FATAL_ERROR "apply-macos-dpi-patch: PATCH_FILE not set")
endif()

execute_process(
  COMMAND git apply --reverse --check "${PATCH_FILE}"
  RESULT_VARIABLE already_applied
  OUTPUT_QUIET ERROR_QUIET)

if(already_applied EQUAL 0)
  message(STATUS "visage macOS backing-scale patch: already applied, skipping")
  return()
endif()

execute_process(
  COMMAND git apply --ignore-whitespace "${PATCH_FILE}"
  RESULT_VARIABLE apply_result)

if(NOT apply_result EQUAL 0)
  message(FATAL_ERROR
    "visage macOS backing-scale patch: git apply failed (result ${apply_result}). "
    "The pin may have moved — re-evaluate ui/visage/patches/ against the new visage.")
endif()

message(STATUS "visage macOS backing-scale patch: applied")
