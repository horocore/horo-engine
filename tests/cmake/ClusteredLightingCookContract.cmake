# Exercise the actual offline executable without catalogs, keys, tools or native compilation.
if(NOT DEFINED COOK OR NOT EXISTS "${COOK}")
    message(FATAL_ERROR "actual clustered cook executable missing")
endif()
execute_process(COMMAND "${COOK}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
if(NOT status EQUAL 2 OR NOT err MATCHES "Usage: HoroEditorClusteredLightingShaderCook")
    message(FATAL_ERROR "clustered cook must require complete explicit host inputs: ${status}: ${err}")
endif()
if(EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "negative-test root unexpectedly exists")
endif()
execute_process(COMMAND "${COOK}" "${TEST_ROOT}/source" "${TEST_ROOT}/output" "${TEST_ROOT}/scratch"
    "${TEST_ROOT}/catalog" "${TEST_ROOT}/signature" "${TEST_ROOT}/trust" "${TEST_ROOT}/dxc"
    "${TEST_ROOT}/validator" "${TEST_ROOT}/cross" "${TEST_ROOT}/xcrun"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
if(NOT status EQUAL 1 OR EXISTS "${TEST_ROOT}" OR err STREQUAL "")
    message(FATAL_ERROR "invalid admission must fail without package/scratch creation: ${status}: ${err}")
endif()
