foreach(case IN ITEMS horo-engine HoroEditor profile inventory headers)
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/network_chaos_boundary"
        -B "${HORO_TEST_BINARY_DIR}/${case}"
        "-DHORO_ENGINE_SOURCE_DIR=${HORO_ENGINE_SOURCE_DIR}" "-DHORO_HOST=${case}" "-DHORO_CASE=${case}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR "Shipping fault-injection dependency unexpectedly accepted: ${case}")
    endif()
    if(case STREQUAL "inventory")
        set(expected "Non-testing network inventory contains HoroNetworkTransportNull")
    elseif(case STREQUAL "headers")
        set(expected "Non-testing network header surface publishes fault injection")
    elseif(case STREQUAL "profile")
        set(expected "HEADLESS_NULL fault injection requires BUILD_TESTING")
    else()
        set(expected "Network chaos product boundary: ${case}")
    endif()
    string(FIND "${output}${error}" "${expected}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "Missing actionable rejection '${expected}': ${output}${error}")
    endif()
endforeach()
