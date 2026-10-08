if(NOT DEFINED HORO_ENGINE_SOURCE_DIR OR
   NOT DEFINED HORO_TEST_BINARY_DIR OR
   NOT DEFINED CMAKE_GENERATOR)
    message(FATAL_ERROR "Extension SDK reconfigure test inputs are incomplete")
endif()

set(test_source_dir "${HORO_TEST_BINARY_DIR}/source")
set(test_build_dir "${HORO_TEST_BINARY_DIR}/build")
file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY
    "${test_source_dir}/include/Horo/Extensions"
    "${test_source_dir}/scripts"
    "${test_source_dir}/sdk/ci"
    "${test_source_dir}/sdk/cmake"
    "${test_source_dir}/sdk/schemas")

configure_file(
    "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/extension_sdk_reconfigure/CMakeLists.txt"
    "${test_source_dir}/CMakeLists.txt"
    COPYONLY)
foreach(source_file IN ITEMS
        LICENSE
        include/Horo/Extensions/ExtensionAbi.h
        include/Horo/Extensions/EditorActivityAbi.h
        scripts/scaffold_extension.py
        scripts/bootstrap_extension_ci.py
        scripts/run_extension_author_ci.py
        sdk/ExtensionSdkREADME.md.in
        sdk/CompatibilityMatrix.json.in
        sdk/MigrationGuide.md.in
        sdk/ci/extension-author-ci.yml
        sdk/ci/extension-ci.lock.json.in
        sdk/extension-sdk.json.in
        sdk/cmake/HoroEngineExtensionSdkConfig.cmake.in
        sdk/schemas/extension-manifest-v1.schema.json)
    configure_file(
        "${HORO_ENGINE_SOURCE_DIR}/${source_file}"
        "${test_source_dir}/${source_file}"
        COPYONLY)
endforeach()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${test_source_dir}"
    -B "${test_build_dir}"
    -G "${CMAKE_GENERATOR}"
    "-DHORO_EXTENSION_SDK_MODULE=${HORO_ENGINE_SOURCE_DIR}/cmake/HoroExtensionSdk.cmake")
if(CMAKE_GENERATOR_PLATFORM)
    list(APPEND configure_command -A "${CMAKE_GENERATOR_PLATFORM}")
endif()
if(CMAKE_GENERATOR_TOOLSET)
    list(APPEND configure_command -T "${CMAKE_GENERATOR_TOOLSET}")
endif()

execute_process(COMMAND ${configure_command} RESULT_VARIABLE first_configure_result)
if(NOT first_configure_result EQUAL 0)
    message(FATAL_ERROR "Initial extension SDK configure failed")
endif()

set(package_root "${test_build_dir}/sdk/extensions/0.1.0")
file(MAKE_DIRECTORY
    "${package_root}/obsolete"
    "${package_root}/include/Horo/Extensions")
file(WRITE "${package_root}/obsolete/removed.txt" "obsolete")
file(WRITE "${package_root}/include/Horo/Extensions/Removed.h" "obsolete")

execute_process(COMMAND ${configure_command} RESULT_VARIABLE second_configure_result)
if(NOT second_configure_result EQUAL 0)
    message(FATAL_ERROR "Reused-build extension SDK configure failed")
endif()

file(GLOB_RECURSE staged_files
    LIST_DIRECTORIES FALSE
    RELATIVE "${package_root}"
    "${package_root}/*")
list(SORT staged_files)
set(expected_files
    LICENSE
    README.md
    bin/horo-extension-author-ci.py
    bin/horo-scaffold-extension.py
    include/Horo/Extensions/ExtensionAbi.h
    include/Horo/Extensions/EditorActivityAbi.h
    lib/cmake/HoroEngineExtensionSdk/HoroEngineExtensionSdkConfig.cmake
    lib/cmake/HoroEngineExtensionSdk/HoroEngineExtensionSdkConfigVersion.cmake
    share/horo/extension-sdk/ci/bootstrap.py
    share/horo/extension-sdk/ci/extension-author-ci.yml
    share/horo/extension-sdk/ci/extension-ci.lock.json
    share/horo/extension-sdk/extension-manifest-v1.schema.json
    share/horo/extension-sdk/extension-sdk.json)
list(APPEND expected_files
    share/horo/extension-sdk/CompatibilityMatrix.json
    share/horo/extension-sdk/MigrationGuide.md)
list(SORT expected_files)

if(NOT staged_files STREQUAL expected_files)
    message(FATAL_ERROR
        "Reused-build SDK artifact set differs from the expected set. "
        "Expected: ${expected_files}; actual: ${staged_files}")
endif()
