# The pinned vendor source is private to the GUI adapter. Keep its cache patch idempotent.
set(horo_zep_viewport_patch "${CMAKE_CURRENT_LIST_DIR}/patches/zep/horo-viewport.patch")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --reverse --check --ignore-space-change
    "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_already_patched OUTPUT_QUIET ERROR_QUIET)
if(NOT horo_zep_already_patched EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --check --ignore-space-change
        "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_patch_check OUTPUT_QUIET ERROR_QUIET)
    if(NOT horo_zep_patch_check EQUAL 0)
        message(FATAL_ERROR "Pinned Zep source does not match the reviewed Horo viewport bridge")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --ignore-space-change
        "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_patch_result OUTPUT_QUIET ERROR_QUIET)
    if(NOT horo_zep_patch_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply the private Horo Zep viewport bridge")
    endif()
endif()
