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

find_program(PATCH_EXECUTABLE patch)

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
