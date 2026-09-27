foreach(required IN ITEMS HORO_EXTENSION_SDK_PACKAGE_DIR PYTHON_EXECUTABLE HORO_TEST_BINARY_DIR)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "Missing extension author CI input: ${required}")
    endif()
endforeach()

file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")
file(MAKE_DIRECTORY "${HORO_TEST_BINARY_DIR}")
set(source_dir "${HORO_TEST_BINARY_DIR}/author-project")
set(output_dir "${source_dir}/ci-artifacts")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(host_platform windows-x64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(host_platform macos-x64)
    else()
        set(host_platform macos-arm64)
    endif()
else()
    set(host_platform linux-x64)
endif()
execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-scaffold-extension.py"
        --shape backend --id com.horo.fixture.author --name "CI Fixture" --output "${source_dir}"
    RESULT_VARIABLE scaffold_result)
if(NOT scaffold_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate source-free author CI fixture")
endif()

execute_process(
    COMMAND "${PYTHON_EXECUTABLE}" "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-author-ci.py"
        --sdk "${HORO_EXTENSION_SDK_PACKAGE_DIR}"
        --project "${source_dir}"
        --output "${output_dir}"
        --platform "${host_platform}"
        --repository horocore/author-fixture
        --commit 0123456789abcdef0123456789abcdef01234567
        --sdk-sha256 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
    RESULT_VARIABLE author_result)
if(NOT author_result EQUAL 0)
    message(FATAL_ERROR "Source-free extension author CI failed")
endif()
foreach(output IN ITEMS extension.horopkg provenance.json ctest.xml)
    if(NOT EXISTS "${output_dir}/${output}")
        message(FATAL_ERROR "Missing extension author artifact: ${output}")
    endif()
endforeach()
file(READ "${output_dir}/provenance.json" provenance)
string(JSON recorded_commit GET "${provenance}" commit)
if(NOT recorded_commit STREQUAL "0123456789abcdef0123456789abcdef01234567")
    message(FATAL_ERROR "Extension author artifact lacks exact source attribution")
endif()
