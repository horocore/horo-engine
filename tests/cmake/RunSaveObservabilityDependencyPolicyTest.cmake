if(NOT HORO_ENGINE_SOURCE_DIR OR NOT HORO_TEST_BINARY_DIR)
    message(FATAL_ERROR "HORO_ENGINE_SOURCE_DIR and HORO_TEST_BINARY_DIR are required")
endif()
foreach(owner none HoroApplication HoroFoundation)
    set(reverse "")
    if(NOT owner STREQUAL "none")
        set(reverse "${owner}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/save_observability_dependency"
        -B "${HORO_TEST_BINARY_DIR}/${owner}"
        "-DHORO_ENGINE_SOURCE_DIR=${HORO_ENGINE_SOURCE_DIR}" "-DHORO_REVERSE_OWNER=${reverse}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(owner STREQUAL "none")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Explicit Save host composition was rejected: ${output}\n${error}")
        endif()
    else()
        if(result EQUAL 0 OR NOT "${output}\n${error}" MATCHES "Dependency direction violation:")
            message(FATAL_ERROR "Reverse ${owner} -> HoroRuntime was not rejected with policy evidence: ${output}\n${error}")
        endif()
    endif()
endforeach()
