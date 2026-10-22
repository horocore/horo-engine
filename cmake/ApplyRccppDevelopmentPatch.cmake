foreach(horo_rccpp_patch IN ITEMS
    "${CMAKE_CURRENT_LIST_DIR}/patches/rccpp/horo-msvc-transport.patch"
    "${CMAKE_CURRENT_LIST_DIR}/patches/rccpp/horo-gui-paths.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${horo_rccpp_patch}")
find_package(Git REQUIRED)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${horo_rccpp_SOURCE_DIR}" apply --reverse --check --unidiff-zero --ignore-space-change
    "${horo_rccpp_patch}" RESULT_VARIABLE horo_rccpp_patched OUTPUT_QUIET ERROR_QUIET)
if(NOT horo_rccpp_patched EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${horo_rccpp_SOURCE_DIR}" apply --check --unidiff-zero --ignore-space-change
        "${horo_rccpp_patch}" RESULT_VARIABLE horo_rccpp_patch_check OUTPUT_QUIET ERROR_QUIET)
    if(NOT horo_rccpp_patch_check EQUAL 0)
        message(FATAL_ERROR "Pinned RCC++ source does not match the reviewed Horo development patch")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${horo_rccpp_SOURCE_DIR}" apply --unidiff-zero --ignore-space-change
        "${horo_rccpp_patch}" RESULT_VARIABLE horo_rccpp_patch_result OUTPUT_QUIET ERROR_QUIET)
    if(NOT horo_rccpp_patch_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply the Horo RCC++ development patch")
    endif()
endif()

endforeach()
