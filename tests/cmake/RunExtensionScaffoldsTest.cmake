foreach(required IN ITEMS HORO_EXTENSION_SDK_PACKAGE_DIR HORO_EXTENSION_SCAFFOLDER
        HORO_EXTENSION_ACTIVATION_TEST HORO_TEST_BINARY_DIR PYTHON_EXECUTABLE)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing extension scaffold test input: ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${HORO_TEST_BINARY_DIR}")

foreach(shape IN ITEMS gui backend script hybrid)
    set(source_dir "${HORO_TEST_BINARY_DIR}/${shape}")
    set(build_dir "${HORO_TEST_BINARY_DIR}/${shape}-build")
    execute_process(
        COMMAND "${PYTHON_EXECUTABLE}" "${HORO_EXTENSION_SCAFFOLDER}" extension
            --shape "${shape}"
            --id "com.horo.fixture.${shape}"
            --name "${shape} fixture"
            --output "${source_dir}"
        RESULT_VARIABLE scaffold_result)
    if(NOT scaffold_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate ${shape} extension scaffold")
    endif()

    set(configure_command
        "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}"
        -G "${CMAKE_GENERATOR}"
        "-DHoroEngineExtensionSdk_DIR=${HORO_EXTENSION_SDK_PACKAGE_DIR}/lib/cmake/HoroEngineExtensionSdk")
    if(CMAKE_GENERATOR_PLATFORM)
        list(APPEND configure_command -A "${CMAKE_GENERATOR_PLATFORM}")
    endif()
    if(CMAKE_GENERATOR_TOOLSET)
        list(APPEND configure_command -T "${CMAKE_GENERATOR_TOOLSET}")
    endif()
    execute_process(COMMAND ${configure_command} RESULT_VARIABLE configure_result)
    if(NOT configure_result EQUAL 0)
        message(FATAL_ERROR "Failed to configure ${shape} extension scaffold")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --config Release
        RESULT_VARIABLE build_result)
    if(NOT build_result EQUAL 0)
        message(FATAL_ERROR "Failed to build ${shape} extension scaffold")
    endif()
    execute_process(
        COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build_dir}" -C Release
            --output-on-failure
        RESULT_VARIABLE test_result)
    if(NOT test_result EQUAL 0)
        message(FATAL_ERROR "Generated ${shape} extension contract tests failed")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${build_dir}" --target package --config Release
        RESULT_VARIABLE package_result)
    if(NOT package_result EQUAL 0)
        message(FATAL_ERROR "Failed to package ${shape} extension scaffold")
    endif()
    file(GLOB package_artifacts "${build_dir}/com.horo.fixture.${shape}-1.0.0*.zip")
    if(NOT package_artifacts)
        message(FATAL_ERROR "The ${shape} scaffold produced no package artifact")
    endif()
    list(GET package_artifacts 0 package_artifact)
    set(package_dir "${HORO_TEST_BINARY_DIR}/${shape}-package")
    file(MAKE_DIRECTORY "${package_dir}")
    file(ARCHIVE_EXTRACT INPUT "${package_artifact}" DESTINATION "${package_dir}")
    execute_process(
        COMMAND "${HORO_EXTENSION_ACTIVATION_TEST}" "${package_dir}" "com.horo.fixture.${shape}"
        RESULT_VARIABLE activation_result)
    if(NOT activation_result EQUAL 0)
        message(FATAL_ERROR "Packaged ${shape} extension failed host activation")
    endif()
endforeach()
