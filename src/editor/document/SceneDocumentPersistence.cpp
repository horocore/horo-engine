#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Scene/PrimitiveCatalog.h"
#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <variant>

namespace Horo::Editor {
    using namespace ScenePersistenceDetail;

    namespace {
        /** @brief Writes, rechecks, and atomically replaces one serialized scene payload. */
        [[nodiscard]] Result<ProjectSceneSaveResult> ReplaceSerializedScene(const std::filesystem::path &absoluteProjectRoot,
                                                                            const std::filesystem::path &absoluteScenePath,
                                                                            const std::string &serialized,
                                                                            const SceneFileFingerprint &expectedFingerprint,
                                                                            const bool overwriteConflict, DurableFileSystem &files) {
            const std::vector<std::byte> bytes = Bytes(serialized);
            std::filesystem::path prepared = absoluteScenePath;
            prepared += ".save.tmp";
            if (Result<void> write = files.WriteDurable(prepared, bytes); write.HasError()) {
                static_cast<void>(files.RemoveDurable(prepared));
                return Result<ProjectSceneSaveResult>::Failure(write.ErrorValue());
            }
            auto currentFingerprint = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
            if (currentFingerprint.HasError()) {
                static_cast<void>(files.RemoveDurable(prepared));
                return Result<ProjectSceneSaveResult>::Failure(currentFingerprint.ErrorValue());
            }
            if (!overwriteConflict && currentFingerprint.Value() != expectedFingerprint) {
                static_cast<void>(files.RemoveDurable(prepared));
                return Result<ProjectSceneSaveResult>::Success({
                    .status = ProjectSceneSaveStatus::Conflict,
                    .fingerprint = std::move(currentFingerprint).Value(),
                });
            }
            if (Result<void> replace = files.AtomicReplace(prepared, absoluteScenePath); replace.HasError()) {
                static_cast<void>(files.RemoveDurable(prepared));
                return Result<ProjectSceneSaveResult>::Failure(replace.ErrorValue());
            }
            return Result<ProjectSceneSaveResult>::Success({
                .status = ProjectSceneSaveStatus::Saved,
                .fingerprint = Fingerprint(serialized),
            });
        }

        /** @brief Validates the metadata and payload of one parsed recovery record. */
        [[nodiscard]] Result<ProjectSceneRecoveryRecord> ParseRecoveryRecord(const std::filesystem::path &absoluteScenePath,
                                                                             const Json &record) {
            if (!record.is_object() || record.value("recordVersion", 0) != 1 || !record.contains("canonicalPath") ||
                !record["canonicalPath"].is_string() || !record.contains("savedRevision") ||
                !record["savedRevision"].is_number_unsigned() || !record.contains("savedState") ||
                !record["savedState"].is_number_unsigned() || !record.contains("recoveredRevision") ||
                !record["recoveredRevision"].is_number_unsigned() || !record.contains("recoveredState") ||
                !record["recoveredState"].is_number_unsigned() || !record.contains("sceneChecksum") ||
                !record["sceneChecksum"].is_string() || !record.contains("scene") || !record["scene"].is_object())
                return Result<ProjectSceneRecoveryRecord>::Failure(PersistenceError(SceneInvalid, "Recovery record schema is incomplete."));
            if (std::filesystem::path{record["canonicalPath"].get<std::string>()}.lexically_normal() !=
                absoluteScenePath.lexically_normal())
                return Result<ProjectSceneRecoveryRecord>::Failure(
                    PersistenceError(SceneInvalid, "Recovery record belongs to a different canonical scene."));
            if (record["sceneChecksum"].get<std::string>() != SceneChecksum(record["scene"]))
                return Result<ProjectSceneRecoveryRecord>::Failure(
                    PersistenceError(SceneInvalid, "Recovery scene checksum does not match its payload."));

            auto scene = ParseScene(record["scene"].dump());
            if (scene.HasError())
                return Result<ProjectSceneRecoveryRecord>::Failure(scene.ErrorValue());
            ParsedScene parsed = std::move(scene).Value();
            ProjectSceneRecoveryRecord result{
                .absoluteCanonicalPath = absoluteScenePath,
                .savedRevision = DocumentRevision{record["savedRevision"].get<std::uint64_t>()},
                .savedState = DocumentStateId{record["savedState"].get<std::uint64_t>()},
                .recoveredRevision = DocumentRevision{record["recoveredRevision"].get<std::uint64_t>()},
                .recoveredState = DocumentStateId{record["recoveredState"].get<std::uint64_t>()},
                .objects = std::move(parsed.objects),
                .prefabInstances = std::move(parsed.prefabInstances),
            };
            if (!result.savedState.IsValid() || !result.recoveredState.IsValid() || result.recoveredRevision < result.savedRevision)
                return Result<ProjectSceneRecoveryRecord>::Failure(
                    PersistenceError(SceneInvalid, "Recovery revision metadata is invalid."));
            return Result<ProjectSceneRecoveryRecord>::Success(std::move(result));
        }

        /** @brief Resolves the configured default scene path from project metadata. */
        [[nodiscard]] Result<std::optional<std::filesystem::path>> ResolveDefaultScenePath(const std::filesystem::path &projectRoot,
                                                                                           const Json &metadata) {
            if (!metadata.is_object() || !metadata.contains("settings") || !metadata["settings"].is_object() ||
                !metadata["settings"].contains("defaultScene") || !metadata["settings"]["defaultScene"].is_string())
                return Result<std::optional<std::filesystem::path>>::Failure(
                    PersistenceError(ScenePathInvalid, "Project metadata does not contain settings.defaultScene."));
            const std::string configuredScene = metadata["settings"]["defaultScene"].get<std::string>();
            if (configuredScene.empty())
                return Result<std::optional<std::filesystem::path>>::Success(std::nullopt);
            const std::filesystem::path relativeScene = std::filesystem::path{configuredScene}.lexically_normal();
            if (!IsSafeProjectRelativePath(relativeScene))
                return Result<std::optional<std::filesystem::path>>::Failure(
                    PersistenceError(ScenePathInvalid, "Project defaultScene must be a safe project-relative path."));
            const std::filesystem::path absoluteScene = (projectRoot / relativeScene).lexically_normal();
            if (!absoluteScene.is_absolute() || !IsResolvedContainedBy(projectRoot, absoluteScene))
                return Result<std::optional<std::filesystem::path>>::Failure(
                    PersistenceError(ScenePathInvalid, "Resolved defaultScene is outside the project root."));
            return Result<std::optional<std::filesystem::path>>::Success(absoluteScene);
        }

        [[nodiscard]] std::string SerializeRecoveryRecord(const std::filesystem::path &absoluteScenePath,
                                                          const SceneDocumentSnapshot &snapshot, const DocumentRevision savedRevision,
                                                          const DocumentStateId savedState) {
            Json scene = SceneJson(snapshot);
            const Json record{
                {"recordVersion", 1},
                {"canonicalPath", absoluteScenePath.string()},
                {"savedRevision", savedRevision.value},
                {"savedState", savedState.value},
                {"recoveredRevision", snapshot.revision.value},
                {"recoveredState", snapshot.state.value},
                {"capturedAtUnixMilliseconds",
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()},
                {"sceneChecksum", SceneChecksum(scene)},
                {"scene", std::move(scene)},
            };
            return record.dump(2) + '\n';
        }

        [[nodiscard]] Result<void> WriteRecoveryPayload(const std::filesystem::path &absoluteProjectRoot, const std::string &serialized,
                                                        DurableFileSystem &files) {
            const std::filesystem::path destination = RecoveryPath(absoluteProjectRoot);
            std::filesystem::path prepared = destination;
            prepared += ".tmp";
            const std::vector<std::byte> bytes = Bytes(serialized);
            if (Result<void> write = files.WriteDurable(prepared, bytes); write.HasError()) {
                static_cast<void>(files.RemoveDurable(prepared));
                return write;
            }
            if (Result<void> replace = files.AtomicReplace(prepared, destination); replace.HasError()) {
                static_cast<void>(files.RemoveDurable(prepared));
                return replace;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc LoadProjectDefaultScene */
    Result<std::optional<LoadedProjectScene>> LoadProjectDefaultScene(const std::filesystem::path &absoluteProjectRoot) {
        const std::filesystem::path metadataPath = absoluteProjectRoot / ".horo/project.json";
        if (std::error_code error; !std::filesystem::exists(metadataPath, error)) {
            if (error) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + metadataPath.string() + "'."));
            }
            return Result<std::optional<LoadedProjectScene>>::Success(std::nullopt);
        }

        auto metadataBytes = ReadBoundedFile(metadataPath, kMaximumProjectMetadataBytes);
        if (metadataBytes.HasError()) {
            return Result<std::optional<LoadedProjectScene>>::Failure(metadataBytes.ErrorValue());
        }

        try {
            const Json metadata = Json::parse(metadataBytes.Value());
            auto resolvedScene = ResolveDefaultScenePath(absoluteProjectRoot, metadata);
            if (resolvedScene.HasError())
                return Result<std::optional<LoadedProjectScene>>::Failure(resolvedScene.ErrorValue());
            if (!resolvedScene.Value().has_value())
                return Result<std::optional<LoadedProjectScene>>::Success(std::nullopt);

            const std::filesystem::path &absoluteScene = *resolvedScene.Value();
            auto loaded = LoadProjectScene(absoluteProjectRoot, absoluteScene);
            if (loaded.HasError()) {
                return Result<std::optional<LoadedProjectScene>>::Failure(loaded.ErrorValue());
            }
            if (!loaded.Value().existed) {
                return Result<std::optional<LoadedProjectScene>>::Failure(
                    PersistenceError(SceneReadFailed, "Configured default scene does not exist at '" + absoluteScene.string() + "'."));
            }
            return Result<std::optional<LoadedProjectScene>>::Success(std::move(loaded).Value());
        } catch (const Json::exception &exception) {
            return Result<std::optional<LoadedProjectScene>>::Failure(
                PersistenceError(SceneInvalid, "Invalid project metadata JSON: " + std::string{exception.what()}));
        }
    }

    /** @copydoc LoadProjectScene */
    Result<LoadedProjectScene> LoadProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath) || absoluteScenePath.extension() != ".horo") {
            return Result<LoadedProjectScene>::Failure(
                PersistenceError(ScenePathInvalid, "Scene load requires an absolute project-contained .horo path."));
        }

        if (std::error_code error; !std::filesystem::exists(absoluteScenePath, error)) {
            if (error) {
                return Result<LoadedProjectScene>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + absoluteScenePath.string() + "'."));
            }
            return Result<LoadedProjectScene>::Success(LoadedProjectScene{absoluteScenePath, {}, {}, false, SceneFileFingerprint{}});
        }
        auto sceneBytes = ReadBoundedFile(absoluteScenePath, kMaximumSceneBytes);
        if (sceneBytes.HasError()) {
            return Result<LoadedProjectScene>::Failure(sceneBytes.ErrorValue());
        }
        auto scene = ParseScene(sceneBytes.Value());
        if (scene.HasError()) {
            return Result<LoadedProjectScene>::Failure(scene.ErrorValue());
        }
        ParsedScene parsed = std::move(scene).Value();
        return Result<LoadedProjectScene>::Success(LoadedProjectScene{absoluteScenePath, std::move(parsed.objects),
                                                                      std::move(parsed.prefabInstances), true,
                                                                      Fingerprint(sceneBytes.Value())});
    }

    /** @copydoc InspectProjectSceneFingerprint */
    Result<SceneFileFingerprint> InspectProjectSceneFingerprint(const std::filesystem::path &absoluteProjectRoot,
                                                                const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<SceneFileFingerprint>::Failure(
                PersistenceError(ScenePathInvalid, "Scene fingerprint inspection requires an absolute project-contained path."));
        }

        if (std::error_code error; !std::filesystem::exists(absoluteScenePath, error)) {
            if (error) {
                return Result<SceneFileFingerprint>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect '" + absoluteScenePath.string() + "'."));
            }
            return Result<SceneFileFingerprint>::Success(SceneFileFingerprint{});
        }
        auto bytes = ReadBoundedFile(absoluteScenePath, kMaximumSceneBytes);
        if (bytes.HasError()) {
            return Result<SceneFileFingerprint>::Failure(bytes.ErrorValue());
        }
        return Result<SceneFileFingerprint>::Success(Fingerprint(bytes.Value()));
    }

    /** @copydoc SaveProjectScene */
    Result<ProjectSceneSaveResult> SaveProjectScene(const std::filesystem::path &absoluteProjectRoot,
                                                    const std::filesystem::path &absoluteScenePath, const SceneDocumentSnapshot &snapshot,
                                                    const SceneFileFingerprint &expectedFingerprint, const bool overwriteConflict,
                                                    const ProjectMutationCoordinator &mutations, DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<ProjectSceneSaveResult>::Failure(
                PersistenceError(ScenePathInvalid, "Scene save destination must be an absolute path inside the project."));
        }

        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Save,
                .operationId = std::format("scene-save-{}", snapshot.revision.value),
            });
            lease.HasError()) {
            return Result<ProjectSceneSaveResult>::Failure(lease.ErrorValue());
        }

        auto currentFingerprint = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
        if (currentFingerprint.HasError()) {
            return Result<ProjectSceneSaveResult>::Failure(currentFingerprint.ErrorValue());
        }
        if (!overwriteConflict && currentFingerprint.Value() != expectedFingerprint) {
            return Result<ProjectSceneSaveResult>::Success(ProjectSceneSaveResult{
                .status = ProjectSceneSaveStatus::Conflict,
                .fingerprint = std::move(currentFingerprint).Value(),
            });
        }

        const std::string serialized = SceneJson(snapshot).dump(2) + '\n';
        return ReplaceSerializedScene(absoluteProjectRoot, absoluteScenePath, serialized, expectedFingerprint, overwriteConflict, files);
    }

    /** @copydoc SaveProjectSceneToPath */
    Result<ProjectSceneDestinationSaveResult> SaveProjectSceneToPath(const std::filesystem::path &absoluteProjectRoot,
                                                                     const std::filesystem::path &absoluteScenePath,
                                                                     const SceneDocumentSnapshot &snapshot, const bool overwriteExisting,
                                                                     const ProjectMutationCoordinator &mutations,
                                                                     DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsResolvedContainedBy(absoluteProjectRoot, absoluteScenePath) || absoluteScenePath.extension() != ".horo") {
            return Result<ProjectSceneDestinationSaveResult>::Failure(
                PersistenceError(ScenePathInvalid, "Scene destination must be an absolute project-contained .horo path."));
        }

        auto initial = InspectProjectSceneFingerprint(absoluteProjectRoot, absoluteScenePath);
        if (initial.HasError()) {
            return Result<ProjectSceneDestinationSaveResult>::Failure(initial.ErrorValue());
        }
        if (initial.Value().exists && !overwriteExisting) {
            return Result<ProjectSceneDestinationSaveResult>::Success({
                .status = ProjectSceneDestinationSaveStatus::DestinationExists,
                .fingerprint = initial.Value(),
            });
        }

        auto saved = SaveProjectScene(absoluteProjectRoot, absoluteScenePath, snapshot, initial.Value(), false, mutations, files);
        if (saved.HasError()) {
            return Result<ProjectSceneDestinationSaveResult>::Failure(saved.ErrorValue());
        }
        return Result<ProjectSceneDestinationSaveResult>::Success({
            .status = saved.Value().status == ProjectSceneSaveStatus::Saved ? ProjectSceneDestinationSaveStatus::Saved
                                                                            : ProjectSceneDestinationSaveStatus::Conflict,
            .fingerprint = std::move(saved).Value().fingerprint,
        });
    }

    /** @copydoc SetProjectDefaultScenePath */
    Result<void> SetProjectDefaultScenePath(const std::filesystem::path &absoluteProjectRoot,
                                            const std::filesystem::path &absoluteScenePath, const ProjectMutationCoordinator &mutations,
                                            DurableFileSystem &files) {
        const std::filesystem::path projectRoot = absoluteProjectRoot.lexically_normal();
        const std::filesystem::path scenePath = absoluteScenePath.lexically_normal();
        if (!projectRoot.is_absolute() || !scenePath.is_absolute() || !IsResolvedContainedBy(projectRoot, scenePath) ||
            scenePath.extension() != ".horo") {
            return Result<void>::Failure(PersistenceError(ScenePathInvalid, "Default scene must be a project-contained .horo file."));
        }
        const std::filesystem::path relativeScene = scenePath.lexically_relative(projectRoot);
        if (!IsSafeProjectRelativePath(relativeScene))
            return Result<void>::Failure(PersistenceError(ScenePathInvalid, "Default scene path is not a safe project-relative path."));

        if (const auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = projectRoot,
                .owner = ProjectMutationOwner::Save,
                .operationId = "set-project-default-scene",
            });
            lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());

        const std::filesystem::path metadataPath = projectRoot / ".horo/project.json";
        const Result<std::string> contents = ReadBoundedFile(metadataPath, kMaximumProjectMetadataBytes);
        if (contents.HasError())
            return Result<void>::Failure(contents.ErrorValue());
        Json metadata;
        try {
            metadata = Json::parse(contents.Value());
        } catch (const Json::exception &) {
            return Result<void>::Failure(PersistenceError(SceneReadFailed, "Project metadata is not valid JSON."));
        }
        if (!metadata.is_object() || !metadata.contains("settings") || !metadata["settings"].is_object() ||
            !metadata["settings"].contains("defaultScene") || !metadata["settings"]["defaultScene"].is_string()) {
            return Result<void>::Failure(PersistenceError(ScenePathInvalid, "Project metadata has no valid settings.defaultScene field."));
        }
        metadata["settings"]["defaultScene"] = relativeScene.generic_string();

        std::filesystem::path prepared = metadataPath;
        prepared += ".save.tmp";
        const std::vector<std::byte> bytes = Bytes(metadata.dump(2) + "\n");
        if (const Result<void> written = files.WriteDurable(prepared, bytes); written.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return written;
        }
        if (const Result<void> replaced = files.AtomicReplace(prepared, metadataPath); replaced.HasError()) {
            static_cast<void>(files.RemoveDurable(prepared));
            return replaced;
        }
        return Result<void>::Success();
    }

    /** @copydoc WriteProjectSceneRecovery */
    Result<void> WriteProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot, const std::filesystem::path &absoluteScenePath,
                                           const SceneDocumentSnapshot &snapshot, const DocumentRevision savedRevision,
                                           const DocumentStateId savedState, const ProjectMutationCoordinator &mutations,
                                           DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsContainedBy(absoluteProjectRoot, absoluteScenePath) || !savedState.IsValid()) {
            return Result<void>::Failure(
                PersistenceError(ScenePathInvalid, "Recovery destination must describe an absolute project scene."));
        }

        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Autosave,
                .operationId = std::format("scene-autosave-{}", snapshot.revision.value),
            });
            lease.HasError()) {
            return Result<void>::Failure(lease.ErrorValue());
        }

        const std::string serialized = SerializeRecoveryRecord(absoluteScenePath, snapshot, savedRevision, savedState);
        if (serialized.size() > kMaximumRecoveryBytes) {
            return Result<void>::Failure(PersistenceError(SceneInvalid, "Recovery record exceeds the supported size limit."));
        }
        return WriteRecoveryPayload(absoluteProjectRoot, serialized, files);
    }

    /** @copydoc InspectProjectSceneRecovery */
    Result<std::optional<ProjectSceneRecoveryRecord>> InspectProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot,
                                                                                  const std::filesystem::path &absoluteScenePath) {
        if (!absoluteProjectRoot.is_absolute() || !absoluteScenePath.is_absolute() ||
            !IsContainedBy(absoluteProjectRoot, absoluteScenePath)) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                PersistenceError(ScenePathInvalid, "Recovery inspection requires an absolute project scene."));
        }

        const std::filesystem::path recoveryPath = RecoveryPath(absoluteProjectRoot);
        if (std::error_code error; !std::filesystem::exists(recoveryPath, error)) {
            if (error) {
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                    PersistenceError(SceneReadFailed, "Unable to inspect recovery path '" + recoveryPath.string() + "'."));
            }
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Success(std::nullopt);
        }

        auto bytes = ReadBoundedFile(recoveryPath, kMaximumRecoveryBytes);
        if (bytes.HasError()) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(bytes.ErrorValue());
        }
        try {
            const Json record = Json::parse(bytes.Value());
            auto parsed = ParseRecoveryRecord(absoluteScenePath, record);
            if (parsed.HasError())
                return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(parsed.ErrorValue());
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Success(
                std::optional<ProjectSceneRecoveryRecord>{std::move(parsed).Value()});
        } catch (const Json::exception &exception) {
            return Result<std::optional<ProjectSceneRecoveryRecord>>::Failure(
                PersistenceError(SceneInvalid, "Invalid recovery JSON: " + std::string{exception.what()}));
        }
    }

    /** @copydoc DiscardProjectSceneRecovery */
    Result<void> DiscardProjectSceneRecovery(const std::filesystem::path &absoluteProjectRoot, const ProjectMutationCoordinator &mutations,
                                             DurableFileSystem &files) {
        if (!absoluteProjectRoot.is_absolute()) {
            return Result<void>::Failure(PersistenceError(ScenePathInvalid, "Recovery cleanup requires an absolute project root."));
        }
        const std::filesystem::path recoveryPath = RecoveryPath(absoluteProjectRoot);
        if (std::error_code error; !std::filesystem::exists(recoveryPath, error)) {
            if (error) {
                return Result<void>::Failure(PersistenceError(SceneReadFailed, "Unable to inspect recovery state."));
            }
            return Result<void>::Success();
        }
        if (auto lease = mutations.TryAcquire(ProjectMutationRequest{
                .projectRoot = absoluteProjectRoot,
                .owner = ProjectMutationOwner::Autosave,
                .operationId = "scene-recovery-discard",
            });
            lease.HasError()) {
            return Result<void>::Failure(lease.ErrorValue());
        }
        return files.RemoveDurable(recoveryPath);
    }
}  // namespace Horo::Editor
