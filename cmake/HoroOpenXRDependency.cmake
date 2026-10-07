include_guard(GLOBAL)

# Native loader dispatch is resolved from the exact host-owned verified library
# lease. Populate only pinned headers here: no ambient loader lookup/linkage,
# runtime discovery, SDK tools or loader threads in a non-XR composition.
function(horo_add_openxr_headers_dependency)
    FetchContent_Declare(horo_openxr_sdk
        GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
        GIT_TAG f2448a8797c85814aa892efc1ab8707900fbcc78 # release-1.1.63
        GIT_SHALLOW FALSE
        SOURCE_SUBDIR .horo-headers-only)
    FetchContent_MakeAvailable(horo_openxr_sdk)

    file(READ "${horo_openxr_sdk_SOURCE_DIR}/LICENSE" horo_openxr_license_text)
    string(REPLACE "\r\n" "\n" horo_openxr_license_text "${horo_openxr_license_text}")
    string(SHA256 horo_openxr_license_digest "${horo_openxr_license_text}")
    if(NOT horo_openxr_license_digest STREQUAL "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30")
        message(FATAL_ERROR "Pinned OpenXR SDK license differs from the reviewed Apache-2.0 notice")
    endif()
    # Only concrete native adapter targets may consume this private SDK boundary.
    add_library(HoroOpenXRHeaders INTERFACE)
    target_include_directories(HoroOpenXRHeaders INTERFACE "${horo_openxr_sdk_SOURCE_DIR}/include")
    target_compile_definitions(HoroOpenXRHeaders INTERFACE XR_NO_PROTOTYPES)
    install(FILES "${horo_openxr_sdk_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses"
        RENAME OpenXR-SDK-1.1.63-LICENSE COMPONENT OpenXRNotices)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/third-party/OpenXR-SDK.txt" CONTENT
"Name: OpenXR SDK headers
Version: 1.1.63
License: Apache-2.0
Source: https://github.com/KhronosGroup/OpenXR-SDK
Commit: f2448a8797c85814aa892efc1ab8707900fbcc78
License-SHA256: cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30
Loader: exact verified host-owned library lease; never ambient SDK linkage
")
    install(FILES "${CMAKE_BINARY_DIR}/third-party/OpenXR-SDK.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" COMPONENT OpenXRNotices)
endfunction()
