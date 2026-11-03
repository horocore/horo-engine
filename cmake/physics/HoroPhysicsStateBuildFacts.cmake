# The configured profile and compilation database are inputs, never imported cache authority.
function(horo_physics_state_json output value)
    string(REPLACE "\\" "\\\\" escaped "${value}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

function(horo_configure_physics_state_build_facts)
    get_target_property(jolt_build_source Jolt SOURCE_DIR)
    get_filename_component(jolt_source "${jolt_build_source}" DIRECTORY)
    set(tool_root "${PROJECT_SOURCE_DIR}/cmake/physics")
    set(facts_root "${CMAKE_CURRENT_BINARY_DIR}/physics-state/$<CONFIG>")

    # Read the actual reviewed declaration instead of maintaining a second pin.
    file(READ "${PROJECT_SOURCE_DIR}/cmake/HoroPhysicsDependency.cmake" dependency)
    string(REGEX MATCH "JoltPhysics/tar.gz/([0-9a-f]+)" revision_match "${dependency}")
    set(revision "${CMAKE_MATCH_1}")
    string(REGEX MATCH "URL_HASH SHA256=([0-9a-f]+)" archive_match "${dependency}")
    set(archive_sha "${CMAKE_MATCH_1}")
    if(NOT revision OR NOT archive_sha)
        message(FATAL_ERROR "Cannot identify the declared canonical Physics dependency")
    endif()
    file(SHA256 "${PROJECT_SOURCE_DIR}/cmake/patches/HoroJoltConstraintReservation.cmake" patch_sha)

    horo_physics_state_json(source_json "${PROJECT_SOURCE_DIR}")
    horo_physics_state_json(build_json "${CMAKE_BINARY_DIR}")
    horo_physics_state_json(jolt_json "${jolt_source}")
    horo_physics_state_json(compiler_json "${CMAKE_CXX_COMPILER_ID}:${CMAKE_CXX_COMPILER_VERSION}:${CMAKE_CXX_COMPILER}")
    horo_physics_state_json(target_json "${CMAKE_SYSTEM_NAME}:${CMAKE_SYSTEM_PROCESSOR}:${CMAKE_CXX_COMPILER_TARGET}:${CMAKE_OSX_ARCHITECTURES}")
    horo_physics_state_json(abi_json "pointer=${CMAKE_SIZEOF_VOID_P};frontend=${CMAKE_CXX_COMPILER_FRONTEND_VARIANT};simulate=${CMAKE_CXX_SIMULATE_ID}")
    file(GENERATE OUTPUT "${facts_root}/profile.json" CONTENT
"{
  \"version\": 1,
  \"sourceRoot\": ${source_json},
  \"buildRoot\": ${build_json},
  \"joltRoot\": ${jolt_json},
  \"solver\": {\"revision\": \"${revision}\", \"archiveSha256\": \"${archive_sha}\", \"patchSha256\": \"${patch_sha}\", \"solverSchema\": 1},
  \"physics\": {\"snapshotSchema\": 2, \"orderingSchema\": 1},
  \"platform\": {\"compiler\": ${compiler_json}, \"target\": ${target_json}, \"abi\": ${abi_json}, \"buildMode\": \"$<CONFIG>\"}
}
")

    # Revalidate actual inputs on every invocation. Atomic output writes preserve
    # object-cache hits when the facts are unchanged, without a second stale inventory.
    add_custom_target(HoroPhysicsStateBuildFacts
        COMMAND "${Python3_EXECUTABLE}" "${tool_root}/collect_physics_state_build_facts.py"
            --profile "${facts_root}/profile.json"
            --compile-commands "${CMAKE_BINARY_DIR}/compile_commands.json"
            --manifest "${facts_root}/manifest.json"
        COMMAND "${Python3_EXECUTABLE}" "${tool_root}/generate_physics_state_build_facts.py"
            --manifest "${facts_root}/manifest.json"
            --header "${facts_root}/HoroPhysicsStateBuildFacts.h"
            --report "${facts_root}/report.json"
        BYPRODUCTS "${facts_root}/HoroPhysicsStateBuildFacts.h"
            "${facts_root}/manifest.json" "${facts_root}/report.json"
        COMMENT "Checking canonical Physics state compatibility inputs from configured recipes"
        VERBATIM)
    add_dependencies(HoroPhysics HoroPhysicsStateBuildFacts)
    target_include_directories(HoroPhysics PRIVATE "${facts_root}")
endfunction()
