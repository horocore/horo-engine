set(_HORO_PROJECT_MIGRATIONS_ROOT "${PROJECT_SOURCE_DIR}/src/application/project_migrations")
set(_HORO_PROJECT_MIGRATIONS_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/project.py")
set(HORO_PROJECT_MIGRATION_CATALOG_CPP "${HORO_GENERATED_DIR}/GeneratedProjectMigrationCatalog.cpp")
set(_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST "${HORO_GENERATED_DIR}/GeneratedProjectMigrationSources.cmake")
set(HORO_PROJECT_MIGRATION_SET_HASH_FILE "${HORO_GENERATED_DIR}/GeneratedProjectMigrationSetHash.txt")
set(HORO_PROJECT_MIGRATION_CATALOG_JSON "${HORO_GENERATED_DIR}/GeneratedProjectMigrationCatalog.json")

file(GLOB_RECURSE _HORO_PROJECT_MIGRATION_INPUTS CONFIGURE_DEPENDS
    "${_HORO_PROJECT_MIGRATIONS_ROOT}/*")

execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_HORO_PROJECT_MIGRATIONS_SCRIPT}" migrations
            "${_HORO_PROJECT_MIGRATIONS_ROOT}"
            "${HORO_PROJECT_MIGRATION_CATALOG_CPP}"
            "${_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST}"
            "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
            "${PROJECT_SOURCE_DIR}/releases/${PROJECT_VERSION}/release.json"
    RESULT_VARIABLE _horo_project_migrations_result
    ERROR_VARIABLE _horo_project_migrations_error
)
if(NOT _horo_project_migrations_result EQUAL 0)
    message(FATAL_ERROR "Project migration catalog generation failed: ${_horo_project_migrations_error}")
endif()

include("${_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST}")

file(READ "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}" _horo_generated_migration_set_hash)
string(STRIP "${_horo_generated_migration_set_hash}" _horo_generated_migration_set_hash)
file(READ "${PROJECT_SOURCE_DIR}/releases/${PROJECT_VERSION}/release.json" _horo_current_release_manifest)
string(JSON _horo_frozen_migration_set_hash ERROR_VARIABLE _horo_release_json_error
       GET "${_horo_current_release_manifest}" migrationDefinitions)
if(_horo_release_json_error OR NOT _horo_frozen_migration_set_hash STREQUAL _horo_generated_migration_set_hash)
    message(FATAL_ERROR
        "Generated project migration definition set differs from frozen release ${PROJECT_VERSION}")
endif()

add_custom_command(
    OUTPUT "${HORO_PROJECT_MIGRATION_CATALOG_CPP}"
           "${_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST}"
           "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}"
           "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
    COMMAND "${Python3_EXECUTABLE}" "${_HORO_PROJECT_MIGRATIONS_SCRIPT}" migrations
            "${_HORO_PROJECT_MIGRATIONS_ROOT}"
            "${HORO_PROJECT_MIGRATION_CATALOG_CPP}"
            "${_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST}"
            "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}"
            "${PROJECT_SOURCE_DIR}/releases/${PROJECT_VERSION}/release.json"
    DEPENDS "${_HORO_PROJECT_MIGRATIONS_SCRIPT}" ${_HORO_PROJECT_MIGRATION_INPUTS}
    COMMENT "Regenerating Horo project migration catalog"
    VERBATIM
)

add_custom_target(HoroProjectMigrationCatalog ALL
    DEPENDS "${HORO_PROJECT_MIGRATION_CATALOG_CPP}"
            "${_HORO_PROJECT_MIGRATION_SOURCE_MANIFEST}"
            "${HORO_PROJECT_MIGRATION_SET_HASH_FILE}"
            "${HORO_PROJECT_MIGRATION_CATALOG_JSON}")
