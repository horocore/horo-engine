set(_HORO_RELEASES_ROOT "${PROJECT_SOURCE_DIR}/releases")
set(_HORO_PROJECT_CONTRACT "${_HORO_RELEASES_ROOT}/${PROJECT_VERSION}/project-contract.json")
set(_HORO_RELEASE_MANIFEST "${_HORO_RELEASES_ROOT}/${PROJECT_VERSION}/release.json")
set(_HORO_PROJECT_COMPATIBILITY_OUTPUT
    "${HORO_GENERATED_DIR}/GeneratedProjectCompatibility.h")
set(_HORO_PROJECT_COMPATIBILITY_SCRIPT
    "${PROJECT_SOURCE_DIR}/scripts/project.py")
file(GLOB_RECURSE _HORO_RELEASE_INPUTS CONFIGURE_DEPENDS "${_HORO_RELEASES_ROOT}/*.json")

if(NOT EXISTS "${_HORO_PROJECT_CONTRACT}")
    message(FATAL_ERROR
        "Missing project compatibility contract for Horo ${PROJECT_VERSION}: ${_HORO_PROJECT_CONTRACT}")
endif()
if(NOT EXISTS "${_HORO_RELEASE_MANIFEST}")
    message(FATAL_ERROR
        "Missing release manifest for Horo ${PROJECT_VERSION}: ${_HORO_RELEASE_MANIFEST}")
endif()

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_HORO_PROJECT_COMPATIBILITY_SCRIPT}" compatibility
            "${_HORO_RELEASES_ROOT}" "${PROJECT_VERSION}" "${_HORO_PROJECT_COMPATIBILITY_OUTPUT}"
            "${_horo_generated_migration_set_hash}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
    RESULT_VARIABLE _horo_project_compatibility_result
    ERROR_VARIABLE _horo_project_compatibility_error
)
if(NOT _horo_project_compatibility_result EQUAL 0)
    message(FATAL_ERROR "Project compatibility generation failed: ${_horo_project_compatibility_error}")
endif()

add_custom_command(
    OUTPUT "${_HORO_PROJECT_COMPATIBILITY_OUTPUT}"
    COMMAND "${Python3_EXECUTABLE}" "${_HORO_PROJECT_COMPATIBILITY_SCRIPT}" compatibility
            "${_HORO_RELEASES_ROOT}" "${PROJECT_VERSION}" "${_HORO_PROJECT_COMPATIBILITY_OUTPUT}"
            "${_horo_generated_migration_set_hash}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
    DEPENDS ${_HORO_RELEASE_INPUTS} "${_HORO_PROJECT_COMPATIBILITY_SCRIPT}"
            "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
    COMMENT "Regenerating Horo project compatibility catalog"
    VERBATIM
)
add_custom_target(HoroProjectCompatibilityCatalog ALL
    DEPENDS "${_HORO_PROJECT_COMPATIBILITY_OUTPUT}")
add_dependencies(HoroProjectCompatibilityCatalog HoroProjectMigrationCatalog)
