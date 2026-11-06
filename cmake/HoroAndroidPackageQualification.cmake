# Explicit package composition: input-only assembly or declared native presentation development profile.
if(NOT ANDROID)
    message(FATAL_ERROR "Android package qualification requires the declared NDK toolchain preset")
endif()
set(HORO_ANDROID_QUALIFICATION_RENDERER "none" CACHE STRING "Explicit Android qualification renderer")
if(NOT HORO_ANDROID_QUALIFICATION_RENDERER STREQUAL "none" AND
   NOT HORO_ANDROID_QUALIFICATION_RENDERER STREQUAL "vulkan-android")
    message(FATAL_ERROR "Android qualification requires the explicit none or vulkan-android profile")
endif()
if(NOT DEFINED ENV{HORO_GAME_ACTIVITY_ROOT})
    message(FATAL_ERROR "Android preflight: extract the verified pinned GameActivity AAR before configure")
endif()
set(_horo_activity "$ENV{HORO_GAME_ACTIVITY_ROOT}/prefab/modules/game-activity_static")
set(_horo_activity_library "${_horo_activity}/libs/android.${ANDROID_ABI}/libgame-activity_static.a")
if(NOT EXISTS "${_horo_activity_library}")
    message(FATAL_ERROR "Android preflight: verified GameActivity native dependency is missing for ${ANDROID_ABI}")
endif()
# Compile the exact shared collector and UTF-8 scalar helper used by ordinary Input.
# This reduced qualification profile exports no SDK and does not instantiate the full engine graph.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroTargetBoundaries.cmake")
add_library(HoroFoundation STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/foundation/text/Utf8.cpp")
add_library(HoroInput STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/input/RawInputCollector.cpp")
target_link_libraries(HoroInput PUBLIC HoroFoundation)
add_library(HoroAndroidInputIntegration STATIC "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/AndroidInput.cpp")
target_link_libraries(HoroAndroidInputIntegration PUBLIC HoroInput)
foreach(target HoroFoundation HoroInput HoroAndroidInputIntegration)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()
if(HORO_ANDROID_QUALIFICATION_RENDERER STREQUAL "vulkan-android")
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroAndroidRendererCore.cmake")
    horo_add_android_renderer_core()
    set(HORO_BUILD_RENDER_VULKAN_ANDROID ON)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroAndroidPresentationTargets.cmake")
    horo_add_android_presentation_targets()
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification"
        "${CMAKE_CURRENT_BINARY_DIR}/android-qualification-composition")
endif()
add_library(HoroAndroidPackageQualification SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification/NativeEntry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/AndroidGameActivityInput.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/AndroidGameActivityEvents.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android/AndroidGameActivitySensors.cpp")
target_compile_features(HoroAndroidPackageQualification PRIVATE cxx_std_20)
target_include_directories(HoroAndroidPackageQualification PRIVATE "${_horo_activity}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/android")
target_link_libraries(HoroAndroidPackageQualification PRIVATE HoroAndroidInputIntegration
    "-Wl,--whole-archive" "${_horo_activity_library}" "-Wl,--no-whole-archive" android log)
set(_horo_qualification_targets HoroFoundation HoroInput HoroAndroidInputIntegration HoroAndroidPackageQualification)
if(HORO_ANDROID_QUALIFICATION_RENDERER STREQUAL "vulkan-android")
    target_sources(HoroAndroidPackageQualification PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification/QualificationHost.cpp")
    target_compile_definitions(HoroAndroidPackageQualification PRIVATE HORO_ANDROID_QUALIFICATION_VULKAN=1)
    target_link_libraries(HoroAndroidPackageQualification PRIVATE HoroPlatformAndroidWindow
        HoroRenderVulkanAndroidHostInterface HoroRenderVulkanAndroid HoroRenderBackendRegistry
        HoroAndroidPresentationAdmissionInternal)
    list(APPEND _horo_qualification_targets HoroVfxSimulationData HoroRenderApi HoroRenderBackendRegistry
        HoroPlatform HoroPlatformAndroidWindow HoroRenderVulkanAndroid HoroAndroidPresentationAdmissionInternal)
endif()
# Header ownership is staged once, after the entire selected target graph exists.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroPublicHeaderOwnership.cmake")
# Every contributing object needs deterministic source/debug paths, including static collector dependencies.
foreach(target IN LISTS _horo_qualification_targets)
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE
        "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=horo-source"
        "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=horo-build"
        "-fdebug-prefix-map=$ENV{HORO_GAME_ACTIVITY_ROOT}=game-activity"
        "-fdebug-prefix-map=$ENV{ANDROID_NDK_ROOT}=android-ndk"
        "-fdebug-compilation-dir=horo-build")
endforeach()
target_link_options(HoroAndroidPackageQualification PRIVATE
    "-Wl,--build-id=sha1" "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined"
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification/exports.map")
set_property(TARGET HoroAndroidPackageQualification APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification/exports.map")
set_target_properties(HoroAndroidPackageQualification PROPERTIES OUTPUT_NAME horo-package-qualification)
