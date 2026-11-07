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
        URL_HASH SHA256=0dbc5e40a01ff142e7e68c03e85247a4dcede2f592d12d3677dee3664d17975a)
    FetchContent_MakeAvailable(nlohmann_json)
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
