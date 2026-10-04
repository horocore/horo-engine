include_guard(GLOBAL)

# Keep upstream's generic option names and flag variables inside this function.
# The Horo adapter consumes Jolt privately; public consumers never inherit its SDK.
function(horo_add_canonical_physics_dependency)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" horo_physics_processor)
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 14)
            message(FATAL_ERROR "Canonical Physics requires macOS 14 or newer")
        endif()
        if(NOT horo_physics_processor MATCHES "^(arm64|aarch64|x86_64|amd64)$")
            message(FATAL_ERROR "Canonical Physics supports macOS arm64/x86_64 only")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if(NOT horo_physics_processor MATCHES "^(x86_64|amd64)$")
            message(FATAL_ERROR "Canonical Physics supports Linux/Windows x86_64 only")
        endif()
    else()
        message(FATAL_ERROR "Canonical Physics is not qualified for this target; use HORO_BUILD_PHYSICS_NATIVE=OFF")
    endif()

    # CanonicalV1 float CPU baseline. This does not claim cross-platform determinism.
    foreach(horo_jolt_option IN ITEMS
            DOUBLE_PRECISION JPH_BUILD_SHARED_LIBS INTERPROCEDURAL_OPTIMIZATION
            OVERRIDE_CXX_FLAGS CPP_EXCEPTIONS_ENABLED CPP_RTTI_ENABLED
            CROSS_PLATFORM_DETERMINISTIC CROSS_COMPILE_ARM ENABLE_ALL_WARNINGS
            USE_ASSERTS FLOATING_POINT_EXCEPTIONS_ENABLED DISABLE_CUSTOM_ALLOCATOR USE_STD_VECTOR
            TRACK_BROADPHASE_STATS TRACK_NARROWPHASE_STATS JPH_TRACK_SIMULATION_STATS
            USE_SSE4_1 USE_SSE4_2 USE_AVX USE_AVX2 USE_AVX512 USE_LZCNT USE_TZCNT
            USE_F16C USE_FMADD USE_WASM_SIMD JPH_USE_WASM64
            JPH_USE_DX12 JPH_USE_VK JPH_USE_MTL JPH_USE_CPU_COMPUTE
            DEBUG_RENDERER_IN_DEBUG_AND_RELEASE DEBUG_RENDERER_IN_DISTRIBUTION
            PROFILER_IN_DEBUG_AND_RELEASE PROFILER_IN_DISTRIBUTION JPH_USE_EXTERNAL_PROFILE
            ENABLE_OBJECT_STREAM ENABLE_INSTALL USE_STATIC_MSVC_RUNTIME_LIBRARY
            TARGET_UNIT_TESTS TARGET_HELLO_WORLD TARGET_PERFORMANCE_TEST TARGET_SAMPLES TARGET_VIEWER)
        set(${horo_jolt_option} OFF)
    endforeach()
    set(OBJECT_LAYER_BITS 16)
    set(GENERATE_DEBUG_SYMBOLS ON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)

    FetchContent_Declare(horo_jolt
        URL https://codeload.github.com/jrouwe/JoltPhysics/tar.gz/e77f175595e64cb44218cc9d9d56fc365ad0e36a
        URL_HASH SHA256=1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR Build
        PATCH_COMMAND "${CMAKE_COMMAND}" "-DJOLT_SOURCE_DIR=<SOURCE_DIR>"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJoltConstraintReservation.cmake"
    )
    FetchContent_MakeAvailable(horo_jolt)

    # PATCH_COMMAND handles fresh population; this verified idempotent pass also covers existing build trees.
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DJOLT_SOURCE_DIR=${horo_jolt_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJoltConstraintReservation.cmake"
        COMMAND_ERROR_IS_FATAL ANY)

    file(SHA256 "${horo_jolt_SOURCE_DIR}/LICENSE" horo_jolt_license_digest)
    if(NOT horo_jolt_license_digest STREQUAL "800abe35d64ad9defd636ff1ee8c961e06f0ebca3ef8d10083e8aa0e8ef86ac3")
        message(FATAL_ERROR "Canonical Jolt license differs from the reviewed v5.6.0 source")
    endif()
    install(FILES "${horo_jolt_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses"
        RENAME JoltPhysics-5.6.0-LICENSE COMPONENT PhysicsNotices)
    file(SHA256 "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/patches/HoroJoltConstraintReservation.cmake" horo_jolt_patch_digest)
    file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/third-party/JoltPhysics.txt" CONTENT
"Name: Jolt Physics
Version: 5.6.0
License: MIT
Source: https://github.com/jrouwe/JoltPhysics
Commit: e77f175595e64cb44218cc9d9d56fc365ad0e36a
Archive-SHA256: 1f32328fb763135de10a244568d6ccb2ed9b1e6593fafe6dc6db5b2719d330bd
License-SHA256: 800abe35d64ad9defd636ff1ee8c961e06f0ebca3ef8d10083e8aa0e8ef86ac3
License-File: JoltPhysics-5.6.0-LICENSE
Horo-Patch: cmake/patches/HoroJoltConstraintReservation.cmake (HORO-1038)
Patch-Purpose: Locked constraint capacity reservation without registration or activation
Patch-SHA256: ${horo_jolt_patch_digest}
")
    install(FILES "${CMAKE_BINARY_DIR}/third-party/JoltPhysics.txt"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" COMPONENT PhysicsNotices)
endfunction()
