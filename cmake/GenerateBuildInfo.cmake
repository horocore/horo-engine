# Generate one reviewed release-notes snapshot and its Welcome projection at build time.
# The snapshot is the distribution payload; neither product reads CHANGELOG.md at runtime.
set(HORO_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated" CACHE INTERNAL "")
set(HORO_RELEASE_NOTES_SNAPSHOT "${HORO_GENERATED_DIR}/ReleaseNotesSnapshot.json" CACHE INTERNAL "")
set(_HORO_CHANGELOG "${PROJECT_SOURCE_DIR}/CHANGELOG.md")
set(_HORO_NOTES_HEADER "${HORO_GENERATED_DIR}/GeneratedBuildInfo.h")
set(_HORO_NOTES_SCRIPT "${PROJECT_SOURCE_DIR}/scripts/parse_changelog.py")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(_HORO_NOTES_COMMAND
    "${Python3_EXECUTABLE}" "${_HORO_NOTES_SCRIPT}"
    --source "${_HORO_CHANGELOG}"
    --version "${HORO_ENGINE_VERSION}"
    --product horo-editor
)

# The generated include must exist for configure-time compiler checks.
execute_process(
    COMMAND ${_HORO_NOTES_COMMAND}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    RESULT_VARIABLE _horo_notes_result
    ERROR_VARIABLE _horo_notes_error
)
if(NOT _horo_notes_result EQUAL 0)
    message(FATAL_ERROR "[GenerateBuildInfo] Cannot freeze reviewed notes: ${_horo_notes_error}")
endif()

add_custom_command(
    OUTPUT "${_HORO_NOTES_HEADER}" "${HORO_RELEASE_NOTES_SNAPSHOT}"
    COMMAND ${_HORO_NOTES_COMMAND}
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    DEPENDS "${_HORO_CHANGELOG}" "${_HORO_NOTES_SCRIPT}"
    COMMENT "Freezing reviewed release notes for Welcome and distribution"
    VERBATIM
)
add_custom_target(HoroBuildInfo ALL DEPENDS "${_HORO_NOTES_HEADER}" "${HORO_RELEASE_NOTES_SNAPSHOT}")
