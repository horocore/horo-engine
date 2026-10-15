if(NOT DEFINED HORO_ENGINE_SOURCE_DIR OR NOT DEFINED HORO_TEST_BINARY_DIR OR
   NOT DEFINED PYTHON_EXECUTABLE OR NOT DEFINED CMAKE_CXX_COMPILER)
    message(FATAL_ERROR "Gameplay input manifest qualification inputs are incomplete")
endif()

# Each execution owns a fresh external project, including spaces in both paths.
file(REMOVE_RECURSE "${HORO_TEST_BINARY_DIR}")
set(source_dir "${HORO_TEST_BINARY_DIR}/Project with spaces")
set(build_dir "${source_dir}/build with spaces")
file(MAKE_DIRECTORY "${source_dir}")
configure_file("${HORO_ENGINE_SOURCE_DIR}/tests/cmake/gameplay_module_input_manifest/CMakeLists.txt"
    "${source_dir}/CMakeLists.txt" COPYONLY)
file(WRITE "${source_dir}/Primary.cpp" "int primary_fixture;\n")
file(WRITE "${source_dir}/Secondary.cpp" "int secondary_fixture;\n")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${source_dir}" -B "${build_dir}" -G Ninja
        "-DHORO_GAMEPLAY_HELPER=${HORO_ENGINE_SOURCE_DIR}/cmake/HoroGameplayModule.cmake"
        "-DPython3_EXECUTABLE=${PYTHON_EXECUTABLE}"
        "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Two-module configure failed: ${output}\n${errors}")
endif()

file(READ "${source_dir}/.horo/local/gameplay_build_inputs.txt" primary_inputs)
file(READ "${build_dir}/Secondary/inputs.txt" secondary_inputs)
if(NOT primary_inputs STREQUAL "Primary.cpp\n" OR NOT secondary_inputs STREQUAL "Secondary.cpp\n")
    message(FATAL_ERROR "Primary freshness metadata was replaced or secondary metadata was lost")
endif()

file(READ "${build_dir}/build.ninja" graph)
foreach(target Primary Secondary)
    string(TOLOWER "${target}" module)
    string(FIND "${graph}" "${target}_generated_behavior_bundle.cpp" bundle_position)
    string(FIND "${graph}" "${target}_generated_behavior_bundle.revision" revision_position)
    string(FIND "${graph}" "--module-id game.${module} --fingerprint fixture-sdk" identity_position)
    if(bundle_position LESS 0 OR revision_position LESS 0 OR identity_position LESS 0)
        message(FATAL_ERROR "${target} lost its generated descriptor/revision/fingerprint recipe")
    endif()
endforeach()
message(STATUS "Two-module configure qualified fresh distinct manifests, descriptors and fingerprints")
