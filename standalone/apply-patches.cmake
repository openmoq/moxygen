# apply-patches.cmake — patch system for FetchContent deps
#
# Call apply_dep_patches(<dep-name> <source-dir>) between
# FetchContent_Populate() and add_subdirectory() to apply any
# patches from standalone/patches/<dep-name>/*.patch
#
# Behavior:
#   Already applied → skip with status message
#   Applies cleanly → apply and log
#   Anything else   → FATAL_ERROR: a patch that stops applying takes the fix it
#                     carries out of the build with it
#
# Patches are applied in filename order (use NNN- prefix for ordering).
#
# Call stash_dep_edits(<dep-name> <git-tag>) before FetchContent_Populate().

find_program(PATCH_EXECUTABLE patch)
find_package(Git QUIET)

# FetchContent moves a checkout to a new pin by stashing local edits and
# popping them afterwards. Applied patches can conflict with the new rev, so a
# tree that is about to move has its edits stashed here, where the pop does not
# follow. They stay recoverable with git stash list.
function(stash_dep_edits DEP_NAME GIT_TAG)
    string(TOUPPER "${DEP_NAME}" _upper)
    set(_src "${FETCHCONTENT_BASE_DIR}/${DEP_NAME}-src")
    if(FETCHCONTENT_SOURCE_DIR_${_upper} OR NOT EXISTS "${_src}/.git"
       OR NOT GIT_EXECUTABLE)
        return()
    endif()
    # A disconnected tree does not move, so its edits stay in place.
    if(FETCHCONTENT_FULLY_DISCONNECTED OR FETCHCONTENT_UPDATES_DISCONNECTED
       OR FETCHCONTENT_UPDATES_DISCONNECTED_${_upper})
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${_src}"
        OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    # Fails when the pin has not been fetched yet, which also means a move.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify --quiet
                "${GIT_TAG}^{commit}"
        WORKING_DIRECTORY "${_src}"
        OUTPUT_VARIABLE _target OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
    )
    if(_head STREQUAL _target)
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain
        WORKING_DIRECTORY "${_src}"
        OUTPUT_VARIABLE _dirty ERROR_QUIET
    )
    if(_dirty STREQUAL "")
        return()
    endif()

    message(STATUS "[patch] ${DEP_NAME}: stashing local edits before moving "
                   "to ${GIT_TAG}")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" stash push --include-untracked --quiet
                -m "moxygen: before moving to ${GIT_TAG}"
        WORKING_DIRECTORY "${_src}"
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "[patch] ${DEP_NAME}: git stash failed in ${_src}. "
                            "Delete it and re-configure.")
    endif()
endfunction()

function(apply_dep_patches DEP_NAME SOURCE_DIR)
    set(_dir "${CMAKE_CURRENT_SOURCE_DIR}/patches/${DEP_NAME}")
    if(NOT IS_DIRECTORY "${_dir}")
        return()
    endif()

    file(GLOB _patches "${_dir}/*.patch")
    if(NOT _patches)
        return()
    endif()
    list(SORT _patches)

    if(NOT PATCH_EXECUTABLE)
        message(FATAL_ERROR
            "[patch] 'patch' not found, but ${_dir} has patches to apply.\n"
            "Install it — standalone/install-system-deps.sh does — and "
            "re-configure.")
    endif()

    foreach(_patch IN LISTS _patches)
        get_filename_component(_name "${_patch}" NAME)

        # A reverse dry-run also succeeds once the change lands upstream and the
        # pin moves past it, which is when the patch should be deleted.
        execute_process(
            COMMAND ${PATCH_EXECUTABLE} -p1 -F0 -R --dry-run -i "${_patch}"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE _rev_rc
            OUTPUT_QUIET ERROR_QUIET
        )
        if(_rev_rc EQUAL 0)
            message(STATUS "[patch] ${DEP_NAME}: ${_name} — already applied")
            continue()
        endif()

        # -F0: patch's default fuzz applies a hunk whose context has drifted,
        # landing a stale patch on code it was never written against.
        execute_process(
            COMMAND ${PATCH_EXECUTABLE} -p1 -F0 --dry-run -i "${_patch}"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE _fwd_rc
            OUTPUT_VARIABLE _fwd_out
            ERROR_VARIABLE _fwd_err
        )
        if(NOT _fwd_rc EQUAL 0)
            message(FATAL_ERROR
                "[patch] ${DEP_NAME}: ${_name} no longer applies to the pinned "
                "${DEP_NAME}.\n${_fwd_out}${_fwd_err}\n"
                "Re-cut it against the rev in build/deps/github_hashes/, or "
                "delete it if the change has landed upstream.")
        endif()

        execute_process(
            COMMAND ${PATCH_EXECUTABLE} -p1 -F0 -i "${_patch}"
            WORKING_DIRECTORY "${SOURCE_DIR}"
            RESULT_VARIABLE _apply_rc
            OUTPUT_VARIABLE _apply_out
            ERROR_VARIABLE _apply_err
        )
        if(NOT _apply_rc EQUAL 0)
            # The tree is half-patched now; the next configure sees neither a
            # clean apply nor an applied patch.
            message(FATAL_ERROR
                "[patch] ${DEP_NAME}: ${_name} failed to apply after a clean "
                "dry-run.\n${_apply_out}${_apply_err}\n"
                "Delete ${SOURCE_DIR} and re-configure.")
        endif()
        message(STATUS "[patch] ${DEP_NAME}: ${_name} — applied")
    endforeach()
endfunction()
