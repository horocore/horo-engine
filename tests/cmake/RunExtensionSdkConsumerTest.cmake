if(NOT DEFINED HORO_EXTENSION_SDK_PACKAGE_DIR OR
   NOT DEFINED HORO_TEST_BINARY_DIR OR
   NOT DEFINED HORO_ENGINE_SOURCE_DIR)
    message(FATAL_ERROR "Extension SDK consumer test inputs are incomplete")
endif()

file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${HORO_TEST_BINARY_DIR}")
file(COPY "${HORO_EXTENSION_SDK_PACKAGE_DIR}/"
    DESTINATION "${HORO_TEST_BINARY_DIR}/sdk")

file(GLOB_RECURSE sdk_text_files LIST_DIRECTORIES FALSE
    "${HORO_TEST_BINARY_DIR}/sdk/*.cmake"
    "${HORO_TEST_BINARY_DIR}/sdk/*.h"
    "${HORO_TEST_BINARY_DIR}/sdk/*.json"
    "${HORO_TEST_BINARY_DIR}/sdk/*.md"
    "${HORO_TEST_BINARY_DIR}/sdk/*.py")
list(APPEND sdk_text_files "${HORO_TEST_BINARY_DIR}/sdk/LICENSE")
foreach(sdk_file IN LISTS sdk_text_files)
    file(READ "${sdk_file}" sdk_contents)
    string(FIND "${sdk_contents}" "${HORO_ENGINE_SOURCE_DIR}" source_path_index)
    if(NOT source_path_index EQUAL -1)
        message(FATAL_ERROR "SDK artifact embeds the engine source path: ${sdk_file}")
    endif()
endforeach()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/extension_sdk_consumer"
    -B "${HORO_TEST_BINARY_DIR}/consumer-build"
    -G "${CMAKE_GENERATOR}"
    "-DHORO_EXPECTED_EXTENSION_SDK_VERSION=${HORO_EXTENSION_SDK_VERSION}"
    "-DHoroEngineExtensionSdk_DIR=${HORO_TEST_BINARY_DIR}/sdk/lib/cmake/HoroEngineExtensionSdk")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND configure_command -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND configure_command -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

# The isolated C consumer must use the exact configured host toolchain, including macOS SDK selection.
foreach(toolchain_option IN ITEMS CMAKE_C_COMPILER CMAKE_LINKER CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET)
    if(DEFINED ${toolchain_option} AND NOT "${${toolchain_option}}" STREQUAL "")
        list(APPEND configure_command "-D${toolchain_option}=${${toolchain_option}}")
    endif()
endforeach()

execute_process(COMMAND ${configure_command} RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "External extension SDK consumer configure failed")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${HORO_TEST_BINARY_DIR}/consumer-build"
        --config Release
    RESULT_VARIABLE build_result)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "External extension SDK consumer build failed")
endif()
