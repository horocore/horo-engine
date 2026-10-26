# Exercise the real executable without tools, signing keys, or native compilation.
if(NOT DEFINED COOK OR NOT EXISTS "${COOK}")
    message(FATAL_ERROR "actual UI cook executable missing")
endif()
execute_process(COMMAND "${COOK}" RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
if(NOT status EQUAL 2 OR NOT err MATCHES "Usage: HoroEditorUiPaintShaderCook")
    message(FATAL_ERROR "UI cook must reject incomplete explicit host inputs: ${status}: ${err}")
endif()
# This root is intentionally never created. Missing independent trust/catalog inputs
# must fail before tools, scratch files, or package publication on every host.
if(EXISTS "${TEST_ROOT}")
    message(FATAL_ERROR "private negative-test root unexpectedly exists")
endif()
execute_process(COMMAND "${COOK}" "${TEST_ROOT}/source.hlsl" "${TEST_ROOT}/output" "${TEST_ROOT}/scratch"
    "${TEST_ROOT}/catalog" "${TEST_ROOT}/signature" "${TEST_ROOT}/trust" "${TEST_ROOT}/dxc"
    "${TEST_ROOT}/validator" "${TEST_ROOT}/cross" "${TEST_ROOT}/xcrun"
    RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 5)
if(NOT status EQUAL 1 OR EXISTS "${TEST_ROOT}" OR err STREQUAL "")
    message(FATAL_ERROR "invalid host admission must fail without creating package/scratch state: ${status}: ${err}")
endif()
