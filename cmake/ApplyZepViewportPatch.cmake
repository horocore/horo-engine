# The pinned vendor source is private to the GUI adapter. Keep its cache patch idempotent.
file(READ "${CMAKE_CURRENT_LIST_DIR}/patches/zep/horo-viewport.patch" horo_zep_viewport_patch_text)
string(REPLACE "\r\n" "\n" horo_zep_viewport_patch_text "${horo_zep_viewport_patch_text}")
# CONFIGURE adds a final LF; expand once through a bracket literal so vendor
# backslashes and template-like text remain unchanged in the generated patch.
string(REGEX REPLACE "\n$" "" horo_zep_viewport_patch_text "${horo_zep_viewport_patch_text}")
set(horo_zep_viewport_patch "${CMAKE_CURRENT_BINARY_DIR}/generated/zep/horo-viewport.patch")
file(CONFIGURE OUTPUT "${horo_zep_viewport_patch}" CONTENT [=[@horo_zep_viewport_patch_text@]=]
    @ONLY NEWLINE_STYLE UNIX)
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --reverse --check --ignore-space-change
    "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_already_patched OUTPUT_QUIET ERROR_QUIET)
if(NOT horo_zep_already_patched EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --check --ignore-space-change
        "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_patch_check OUTPUT_QUIET ERROR_VARIABLE horo_zep_patch_check_error)
    if(NOT horo_zep_patch_check EQUAL 0)
        message(FATAL_ERROR "Pinned Zep source does not match the reviewed Horo viewport bridge: ${horo_zep_patch_check_error}")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${zep_SOURCE_DIR}" apply --ignore-space-change
        "${horo_zep_viewport_patch}" RESULT_VARIABLE horo_zep_patch_result OUTPUT_QUIET ERROR_VARIABLE horo_zep_patch_error)
    if(NOT horo_zep_patch_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply the private Horo Zep viewport bridge: ${horo_zep_patch_error}")
    endif()
endif()
