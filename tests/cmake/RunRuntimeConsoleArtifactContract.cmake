# CTest supplies paths from the native fixture and archive build targets.
set(artifact_size_limit 536870912)
foreach(kind IN ITEMS binary archive)
    set(${kind}_sha256 "")
    if(EXISTS "${${kind}}" AND NOT IS_DIRECTORY "${${kind}}")
        file(SIZE "${${kind}}" size)
        if(size GREATER 0 AND size LESS_EQUAL artifact_size_limit)
            file(SHA256 "${${kind}}" ${kind}_sha256)
        endif()
    endif()
endforeach()

set(manifest "${report}.manifest")
set(execution_result unavailable)
if(NOT binary_sha256 STREQUAL "" AND NOT archive_sha256 STREQUAL "")
    get_filename_component(binary_directory "${binary}" DIRECTORY)
    execute_process(COMMAND "${binary}"
        WORKING_DIRECTORY "${binary_directory}"
        TIMEOUT 30
        OUTPUT_FILE "${manifest}"
        ERROR_QUIET
        RESULT_VARIABLE execution_result)
    if(NOT execution_result MATCHES "^-?[0-9]+$")
        set(execution_result unavailable)
    endif()
endif()

execute_process(COMMAND "${python}" "${verifier}"
    --profile "${profile}"
    --binary "${binary}" --archive "${archive}" --manifest "${manifest}"
    "--binary-sha256=${binary_sha256}" "--archive-sha256=${archive_sha256}"
    "--execution-result=${execution_result}" --report "${report}"
    RESULT_VARIABLE verification_result)
if(NOT verification_result STREQUAL "0")
    message(FATAL_ERROR "Console artifact qualification did not pass (result ${verification_result})")
endif()
