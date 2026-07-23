# Apply the visage patches to the pinned checkout, used as the visage FetchContent
# PATCH_COMMAND. Invoked as:
#   cmake -DPATCH_DIR=<abs dir> -P apply-visage-patches.cmake
# with the working directory set to the visage source tree (FetchContent runs
# PATCH_COMMAND in <SOURCE_DIR>).
#
# The script GLOBS `NNNN-*.patch` from PATCH_DIR (sorted) instead of taking a list
# argument: ExternalProject serialises PATCH_COMMAND arguments through the sub-build,
# where an embedded CMake list separator (`;`) splits one `-DPATCH_FILES=a;b` argument
# into `-DPATCH_FILES=a` plus a stray positional `b` that `cmake -P` silently ignores.
# A single directory argument has no separator to mangle.
#
# IDEMPOTENCY BY RESET, NOT BY REVERSE-CHECK. FetchContent re-runs PATCH_COMMAND on
# every re-configure (not just the first populate), so the step MUST be safe to run on
# an already-patched tree. The previous "git apply --reverse --check to detect applied,
# then skip" approach breaks once two patches touch the SAME file in nearby regions:
# reversing one patch no longer matches because the other shifted its context, so the
# check reports "not applied", the forward apply then fails on lines that are already
# there, and the whole step errors. Instead we HARD RESET the pinned checkout to its
# committed state (the visage pin is this repo's HEAD) and re-apply every patch fresh.
# That is unconditionally idempotent regardless of how the patches overlap. The pin hash
# itself is never changed; `git checkout` only discards our own prior patch application.

if(NOT DEFINED PATCH_DIR)
  message(FATAL_ERROR "apply-visage-patches: PATCH_DIR not set")
endif()

file(GLOB PATCH_FILES "${PATCH_DIR}/[0-9][0-9][0-9][0-9]-*.patch")
list(SORT PATCH_FILES)

if(NOT PATCH_FILES)
  message(FATAL_ERROR "apply-visage-patches: no NNNN-*.patch files found in ${PATCH_DIR}")
endif()

# Reset the checkout to the pinned commit so a re-run starts from a clean, unpatched
# tree (see IDEMPOTENCY note above). Working dir == the visage source tree.
execute_process(
  COMMAND git checkout -- .
  RESULT_VARIABLE reset_result
  OUTPUT_QUIET ERROR_VARIABLE reset_err)
if(NOT reset_result EQUAL 0)
  message(FATAL_ERROR "apply-visage-patches: could not reset the visage checkout: ${reset_err}")
endif()

foreach(patch IN LISTS PATCH_FILES)
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
