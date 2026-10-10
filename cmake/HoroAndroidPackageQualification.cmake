# Explicit packaging qualification composition; does not instantiate the interactive engine host.
if(NOT ANDROID)
    message(FATAL_ERROR "Android package qualification requires the declared NDK toolchain preset")
endif()
if(NOT DEFINED ENV{HORO_GAME_ACTIVITY_ROOT})
    message(FATAL_ERROR "Android preflight: extract the verified pinned GameActivity AAR before configure")
endif()
set(_horo_activity "$ENV{HORO_GAME_ACTIVITY_ROOT}/prefab/modules/game-activity_static")
set(_horo_activity_library "${_horo_activity}/libs/android.${ANDROID_ABI}/libgame-activity_static.a")
if(NOT EXISTS "${_horo_activity_library}")
    message(FATAL_ERROR "Android preflight: verified GameActivity native dependency is missing for ${ANDROID_ABI}")
endif()
add_library(HoroAndroidPackageQualification SHARED
    "${CMAKE_CURRENT_SOURCE_DIR}/apps/android-package-qualification/NativeEntry.cpp")
target_compile_features(HoroAndroidPackageQualification PRIVATE cxx_std_20)
target_include_directories(HoroAndroidPackageQualification PRIVATE "${_horo_activity}/include")
target_link_libraries(HoroAndroidPackageQualification PRIVATE
    "-Wl,--whole-archive" "${_horo_activity_library}" "-Wl,--no-whole-archive" android log)
target_compile_options(HoroAndroidPackageQualification PRIVATE
    "-ffile-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=horo-source"
    "-ffile-prefix-map=${CMAKE_CURRENT_BINARY_DIR}=horo-build"
    "-fdebug-prefix-map=$ENV{HORO_GAME_ACTIVITY_ROOT}=game-activity"
    "-fdebug-prefix-map=$ENV{ANDROID_NDK_ROOT}=android-ndk"
    "-fdebug-compilation-dir=horo-build")
target_link_options(HoroAndroidPackageQualification PRIVATE
    "-Wl,--build-id=sha1" "-Wl,-z,max-page-size=16384" "-Wl,--no-undefined")
set_target_properties(HoroAndroidPackageQualification PROPERTIES OUTPUT_NAME horo-package-qualification)
