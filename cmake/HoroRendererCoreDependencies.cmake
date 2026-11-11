# Exact existing renderer dependencies shared by full and Android qualification graphs.
include_guard(GLOBAL)
include(FetchContent)

set(HORO_NLOHMANN_JSON_REVISION "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03")
set(HORO_VULKAN_HEADERS_REVISION "29f979ee5aa58b7b005f805ea8df7a855c39ff37")

function(horo_populate_renderer_json)
    if(TARGET nlohmann_json::nlohmann_json)
        return()
    endif()
    set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
    set(JSON_Install OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(nlohmann_json
        URL https://codeload.github.com/nlohmann/json/tar.gz/${HORO_NLOHMANN_JSON_REVISION}
        URL_HASH SHA256=0dbc5e40a01ff142e7e68c03e85247a4dcede2f592d12d3677dee3664d17975a
        PATCH_COMMAND "${CMAKE_COMMAND}" "-DHORO_JSON_SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJsonLexerAllocation.cmake")
    FetchContent_MakeAvailable(nlohmann_json)
    # Also verify and patch sources populated before PATCH_COMMAND was added.
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DHORO_JSON_SOURCE_DIR=${nlohmann_json_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJsonLexerAllocation.cmake"
        COMMAND_ERROR_IS_FATAL ANY)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJsonLexerAllocation.cmake" json_patch_digest)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/third-party/nlohmann-json-patch.txt" CONTENT
"Source-Commit: ${HORO_NLOHMANN_JSON_REVISION}
Archive-SHA256: 0dbc5e40a01ff142e7e68c03e85247a4dcede2f592d12d3677dee3664d17975a
Horo-Patch: cmake/patches/HoroJsonLexerAllocation.cmake
Patch-SHA256: ${json_patch_digest}
Patch-Purpose: Propagate checked-STL lexer/parser/array allocation failures and avoid empty cleanup allocation
")
endfunction()

function(horo_populate_renderer_vulkan_headers)
    if(TARGET Vulkan::Headers)
        return()
    endif()
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG "${HORO_VULKAN_HEADERS_REVISION}"
        GIT_SHALLOW FALSE)
    FetchContent_MakeAvailable(VulkanHeaders)
endfunction()
