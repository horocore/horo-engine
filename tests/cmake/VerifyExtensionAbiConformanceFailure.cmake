if(NOT EXISTS "${CONFORMANCE}")
    message(FATAL_ERROR "Extension ABI conformance executable is missing: ${CONFORMANCE}")
endif()

execute_process(
    COMMAND "${CONFORMANCE}" --json "${MODULE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "Incompatible ABI fixture unexpectedly passed: ${output}")
endif()

if(NOT output MATCHES "\"passed\":false,\"code\":\"extension.abi.negotiation_rejected\"")
    message(FATAL_ERROR "Unexpected ABI conformance failure output: ${output}${error}")
endif()
