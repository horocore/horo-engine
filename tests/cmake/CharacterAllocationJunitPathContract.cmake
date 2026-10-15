# Real CTest output-path mechanics only; mandatory Release independently runs the canonical strict verifier.
if(NOT DEFINED TEST_ROOT OR NOT DEFINED CTEST_COMMAND OR NOT DEFINED PYTHON_EXECUTABLE OR NOT DEFINED VERIFIER)
    message(FATAL_ERROR "Missing CTest path contract inputs")
endif()
set(case_names
    "HoroPhysicsTests::Character preparation allocation failures roll back every retained storage stage"
)
set(test_directory "${TEST_ROOT}/build/ci-audio-release")
file(MAKE_DIRECTORY "${test_directory}")
file(WRITE "${test_directory}/CTestTestfile.cmake" "")
foreach(name IN LISTS case_names)
    file(APPEND "${test_directory}/CTestTestfile.cmake"
        "add_test([=[${name}]=] [=[${CMAKE_COMMAND}]=] -E true)\n")
endforeach()
set(report "${test_directory}/character-allocation-ctest.xml")
file(REMOVE "${report}")
execute_process(COMMAND "${CTEST_COMMAND}" --test-dir "build/ci-audio-release"
    --no-tests=error --output-junit "character-allocation-ctest.xml"
    WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT EXISTS "${report}")
    message(FATAL_ERROR "CTest JUnit path contract failed: ${result} ${output} ${error}")
endif()
if(EXISTS "${test_directory}/build/ci-audio-release/character-allocation-ctest.xml")
    message(FATAL_ERROR "CTest incorrectly nested the invocation-relative output path")
endif()
file(READ "${report}" document)
string(REGEX MATCHALL "<testcase[ >]" actual_cases "${document}")
list(LENGTH actual_cases actual_count)
list(LENGTH case_names expected_count)
if(NOT actual_count EQUAL expected_count OR document MATCHES "<(failure|error|skipped)[ />]")
    message(FATAL_ERROR "CTest mechanics fixture did not execute every exact positive case")
endif()
message(STATUS "CTest JUnit path contract: ${actual_count} positive cases at ${report}")

# Exercise the actual reader at the caller's working directory: a bare basename
# must fail, while the producer's resolved test-directory path must qualify.
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${VERIFIER}" --output-junit "character-allocation-ctest.xml"
    WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE misplaced OUTPUT_QUIET ERROR_QUIET)
if(misplaced EQUAL 0)
    message(FATAL_ERROR "The reader incorrectly accepted an invocation-relative basename")
endif()
execute_process(COMMAND "${PYTHON_EXECUTABLE}" "${VERIFIER}" --output-junit "${report}"
    WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE verified OUTPUT_VARIABLE verification ERROR_VARIABLE verification_error)
if(NOT verified EQUAL 0)
    message(FATAL_ERROR "Actual strict Character reader failed: ${verification} ${verification_error}")
endif()
