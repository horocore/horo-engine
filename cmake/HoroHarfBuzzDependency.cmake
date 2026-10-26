include(FetchContent)
include(HoroDependencySdk)

# Keep the pinned source recipe and SDK identity together. The exportable wrapper
# retains the gameplay SDK target name without leaking a second imported target.
function(horo_add_harfbuzz_dependency)
    set(HORO_HARFBUZZ_REVISION "7497c4147469fd4102a7229222586ad5c743c5a1")
    set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
    set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_CORETEXT OFF CACHE BOOL "" FORCE)
    set(HB_HAVE_GOBJECT OFF CACHE BOOL "" FORCE)
    # Optional platform integrations change both code and static-link closure.
    # Keep custom local compositions on the upstream source path.
    if(NOT BUILD_SHARED_LIBS AND NOT HB_HAVE_UNISCRIBE AND NOT HB_HAVE_GDI
       AND NOT HB_HAVE_DIRECTWRITE AND NOT HB_HAVE_INTROSPECTION)
        horo_prepare_dependency_sdk(HarfBuzz "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
            sdk_root sdk_identity sdk_archive)
    endif()
    set(sdk_library "${sdk_root}/lib/harfbuzz${CMAKE_STATIC_LIBRARY_SUFFIX}")
    if(sdk_root AND EXISTS "${sdk_library}" AND EXISTS "${sdk_root}/include/hb.h"
       AND EXISTS "${sdk_root}/include/hb-features.h" AND EXISTS "${sdk_root}/share/COPYING")
        add_library(harfbuzz INTERFACE)
        target_link_libraries(harfbuzz INTERFACE "${sdk_library}")
        target_include_directories(harfbuzz SYSTEM INTERFACE "${sdk_root}/include")
        if(NOT MSVC)
            set(THREADS_PREFER_PTHREAD_FLAG ON)
            find_package(Threads REQUIRED)
            if(CMAKE_USE_PTHREADS_INIT)
                target_link_libraries(harfbuzz INTERFACE Threads::Threads)
            endif()
        endif()
        return()
    endif()

    FetchContent_Declare(harfbuzz
        URL https://codeload.github.com/harfbuzz/harfbuzz/tar.gz/${HORO_HARFBUZZ_REVISION}
        URL_HASH SHA256=6af13e0f154d52a60328a278de00cab26277a616958173a129d91d8f2e8e0284)
    FetchContent_MakeAvailable(harfbuzz)
    # The gameplay SDK links this archive into modules on every source build.
    set_target_properties(harfbuzz PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(HORO_BUILD_DEPENDENCY_SDK_PACKAGES AND sdk_identity)
        # Use upstream's public header inventory, including its generated feature
        # header, rather than packaging private headers or recreating that list.
        get_directory_property(sdk_headers DIRECTORY "${harfbuzz_SOURCE_DIR}" DEFINITION project_headers)
        list(APPEND sdk_headers "${harfbuzz_BINARY_DIR}/src/hb-features.h")
        set(stage "${CMAKE_BINARY_DIR}/dependency-sdk/staging/${sdk_archive}")
        add_custom_target(horo-package-harfbuzz
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${stage}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage}/lib" "${stage}/include" "${stage}/share"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:harfbuzz>"
                "${stage}/lib/harfbuzz${CMAKE_STATIC_LIBRARY_SUFFIX}"
            COMMAND "${CMAKE_COMMAND}" -E copy ${sdk_headers} "${stage}/include"
            COMMAND "${CMAKE_COMMAND}" -E copy "${harfbuzz_SOURCE_DIR}/COPYING" "${stage}/share/COPYING"
            COMMAND "${HORO_DEPENDENCY_SDK_PYTHON}"
                "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.github/scripts/dependency_package.py"
                pack --root "${stage}" --identity "${sdk_identity}"
                --output "${CMAKE_BINARY_DIR}/dependency-sdk/packages/${sdk_archive}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
            DEPENDS harfbuzz VERBATIM)
        add_dependencies(horo-package-dependencies horo-package-harfbuzz)
    endif()
endfunction()
