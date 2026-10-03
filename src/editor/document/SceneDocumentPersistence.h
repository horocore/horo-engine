#pragma once

/**
 * @file SceneDocumentPersistence.h
 * @brief Durable project-scene loading and atomic save contracts.
 */

#include "Horo/Editor/ProjectMutation.h"
#include "editor/document/SceneDocument.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Exact canonical scene-file identity captured from its durable bytes. */
    struct SceneFileFingerprint {
        bool exists{};
        std::uintmax_t byteSize{};
        std::string checksum;

        [[nodiscard]] bool operator==(const SceneFileFingerprint &) const noexcept = default;
    };

    /** @brief Parsed default-scene state resolved to one absolute project-contained path. */
    struct LoadedProjectScene {
        std::filesystem::path absolutePath;
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
        bool existed{};
        SceneFileFingerprint fingerprint;
    };

    /** @brief Conflict-aware outcome of one canonical scene save attempt. */
    enum class ProjectSceneSaveStatus : std::uint8_t {
        Saved,
        Conflict,
    };

    /** @brief Save result carrying the new canonical identity only after a successful replacement. */
    struct ProjectSceneSaveResult {
        ProjectSceneSaveStatus status{ProjectSceneSaveStatus::Conflict};
        SceneFileFingerprint fingerprint;
    };

    /** @brief Outcome of writing a scene snapshot to a user-selected destination. */
    enum class ProjectSceneDestinationSaveStatus : std::uint8_t {
        Saved,
        DestinationExists,
        Conflict,
    };

    /** @brief Destination-save result with the new durable identity on success. */
    struct ProjectSceneDestinationSaveResult {
        ProjectSceneDestinationSaveStatus status{ProjectSceneDestinationSaveStatus::Conflict};
        SceneFileFingerprint fingerprint;
    };

    /** @brief Validated autosave recovery state that never mutates the canonical scene implicitly. */
    struct ProjectSceneRecoveryRecord {
        std::filesystem::path absoluteCanonicalPath;
        DocumentRevision savedRevision;
        DocumentStateId savedState;
        DocumentRevision recoveredRevision;
        DocumentStateId recoveredState;
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
    };

    /**
     * @brief Resolves and parses the project's configured default scene.
     * @param absoluteProjectRoot Absolute project root containing `.horo/project.json`.
     * @return Parsed existing scene, an empty optional when project metadata is absent or its
     * default scene is empty, or a typed validation/read error. A configured but missing default
     * scene is a read error.
     */
    [[nodiscard]] Result<std::optional<LoadedProjectScene>> LoadProjectDefaultScene(const std::filesystem::path &absoluteProjectRoot);

    /**
     * @brief Resolves and parses one explicit absolute project scene.
     * @param absoluteProjectRoot Absolute project root that owns the scene.
     * @param absoluteScenePath Absolute project-contained `.horo` path.
     * @return Parsed scene state, including an explicit missing-file projection, or
     * a typed validation/read error.
     */
    [[nodiscard]] Result<LoadedProjectScene> LoadProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                              const std::filesystem::path &absoluteScenePath);

    /**
     * @brief Durably commits a captured scene snapshot through the shared project-writer boundary.
     * @param absoluteProjectRoot Absolute project root that owns the destination.
     * @param absoluteScenePath Absolute project-contained canonical scene path.
     * @param snapshot Immutable revision/state pair and object values to serialize.
     * @param expectedFingerprint Canonical identity last read or written by this document session.
     * @param overwriteConflict True only after an explicit user decision to replace changed external
     * bytes.
     * @param mutations Shared cross-process project mutation coordinator.
     * @param files Durable filesystem implementation used for write and atomic replacement.
     * @return Saved with the new identity, Conflict without mutation, or a typed persistence error.
     */
    [[nodiscard]] Result<ProjectSceneSaveResult> SaveProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                                  const std::filesystem::path &absoluteScenePath,
                                                                  const SceneDocumentSnapshot &snapshot,
                                                                  const SceneFileFingerprint &expectedFingerprint, bool overwriteConflict,
                                                                  const ProjectMutationCoordinator &mutations, DurableFileSystem &files);

    /**
     * @brief Durably writes a scene snapshot to a new user-selected project path.
     * @param absoluteProjectRoot Absolute project root that owns the destination.
     * @param absoluteScenePath Absolute project-contained destination ending in `.horo`.
     * @param snapshot Immutable scene snapshot to serialize.
     * @param overwriteExisting True only after explicit overwrite confirmation.
     * @param mutations Shared cross-process project mutation coordinator.
     * @param files Durable filesystem implementation.
     * @return Saved, DestinationExists without mutation, Conflict when the destination
     * changed during the operation, or a typed persistence error.
     */
    [[nodiscard]] Result<ProjectSceneDestinationSaveResult> SaveProjectSceneToPath(const std::filesystem::path &absoluteProjectRoot,
                                                                                   const std::filesystem::path &absoluteScenePath,
                                                                                   const SceneDocumentSnapshot &snapshot,
                                                                                   bool overwriteExisting,
                                                                                   const ProjectMutationCoordinator &mutations,
                                                                                   DurableFileSystem &files);

    /**
     * @brief Sets the project default scene to a saved, project-relative scene path.
     * @param absoluteProjectRoot Absolute project root containing `.horo/project.json`.
     * @param absoluteScenePath Absolute project-contained `.horo` path to make the default.
     * @param mutations Shared project mutation coordinator.
     * @param files Durable filesystem implementation used for atomic metadata replacement.
     * @return Success after metadata is durably updated, or a typed validation/I/O error.
     */
    [[nodiscard]] Result<void> SetProjectDefaultScenePath(const std::filesystem::path &absoluteProjectRoot,
                                                          const std::filesystem::path &absoluteScenePath,
                                                          const ProjectMutationCoordinator &mutations, DurableFileSystem &files);

    /**
     * @brief Captures the bounded byte identity of a canonical scene without parsing or mutation.
     * @param absoluteProjectRoot Absolute project root that owns the scene.
     * @param absoluteScenePath Absolute project-contained canonical scene path.
     * @return Existing or missing-file fingerprint, or a typed read/path error.
     */
    [[nodiscard]] Result<SceneFileFingerprint> InspectProjectSceneFingerprint(const std::filesystem::path &absoluteProjectRoot,
                                                                              const std::filesystem::path &absoluteScenePath);

    /**
     * @brief Writes or replaces the bounded recovery record for one dirty default scene.
     * @param absoluteProjectRoot Absolute project root that owns recovery storage.
     * @param absoluteScenePath Absolute canonical scene path represented by the recovery record.
     * @param snapshot Immutable dirty scene snapshot.
     * @param savedRevision Last successfully saved canonical revision.
     * @param savedState Last successfully saved canonical authored-state identity.
     * @param mutations Shared project mutation coordinator.
     * @param files Durable filesystem implementation.
     * @return Success only after the separate recovery artifact is durable.
     */
    [[nodiscard]] Result<void> WriteProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot,
                                                         const std::filesystem::path &absoluteScenePath,
                                                         const SceneDocumentSnapshot &snapshot, DocumentRevision savedRevision,
                                                         DocumentStateId savedState, const ProjectMutationCoordinator &mutations,
                                                         DurableFileSystem &files);

    /**
     * @brief Inspects and validates a recovery record without restoring or modifying canonical state.
     * @param absoluteProjectRoot Absolute project root that owns recovery storage.
     * @param absoluteScenePath Expected absolute canonical scene path.
     * @return Validated record, empty optional when absent, or a typed corruption/read error.
     */
    [[nodiscard]] Result<std::optional<ProjectSceneRecoveryRecord>> InspectProjectSceneRecovery(
        const std::filesystem::path &absoluteProjectRoot, const std::filesystem::path &absoluteScenePath);

    /**
     * @brief Durably removes the default-scene recovery artifact after save or explicit discard.
     * @param absoluteProjectRoot Absolute project root that owns recovery storage.
     * @param mutations Shared project mutation coordinator.
     * @param files Durable filesystem implementation.
     * @return Success when absent or durably removed.
     */
    [[nodiscard]] Result<void> DiscardProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot,
                                                           const ProjectMutationCoordinator &mutations, DurableFileSystem &files);
}  // namespace Horo::Editor
