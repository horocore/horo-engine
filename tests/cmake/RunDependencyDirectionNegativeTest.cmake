if(NOT HORO_ENGINE_SOURCE_DIR OR NOT HORO_TEST_BINARY_DIR)
    message(FATAL_ERROR "HORO_ENGINE_SOURCE_DIR and HORO_TEST_BINARY_DIR are required")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/dependency_direction"
        -B "${HORO_TEST_BINARY_DIR}"
        "-DHORO_ENGINE_SOURCE_DIR=${HORO_ENGINE_SOURCE_DIR}"
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)

set(combined_output "${configure_output}\n${configure_error}")
if(configure_result EQUAL 0)
    message(FATAL_ERROR
        "Representative reverse dependency unexpectedly configured successfully:\n${combined_output}")
endif()

string(FIND "${combined_output}" "Dependency direction violation:" diagnostic_position)
string(FIND "${combined_output}" "HoroFoundation" dependent_position)
string(FIND "${combined_output}" "HoroEditorServices" dependency_position)
if(diagnostic_position EQUAL -1 OR dependent_position EQUAL -1 OR dependency_position EQUAL -1)
    message(FATAL_ERROR
        "Reverse dependency failed without the required actionable diagnostic:\n${combined_output}")
endif()

# Exercise the real permanent policy: neutral data is allowed, verifier/application authority is not.
foreach(case envelope provider package-security app)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/dependency_direction"
        -B "${HORO_TEST_BINARY_DIR}/security-${case}"
        "-DHORO_ENGINE_SOURCE_DIR=${HORO_ENGINE_SOURCE_DIR}" "-DHORO_DEPENDENCY_CASE=${case}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(case STREQUAL "envelope")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Neutral envelope policy unexpectedly failed: ${output}\n${error}")
        endif()
    else()
        if(result EQUAL 0)
            message(FATAL_ERROR "Forbidden Packages ${case} dependency unexpectedly passed")
        endif()
        set(combined "${output}\n${error}")
        if(NOT combined MATCHES "Dependency direction violation:" OR NOT combined MATCHES "HoroPackages")
            message(FATAL_ERROR "Forbidden ${case} edge failed without its owning diagnostic: ${combined}")
        endif()
    endif()
endforeach()
