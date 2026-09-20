#include "editor/screens/workspace/EditorWorkspaceController.h"

#include "Horo/Assets/AssetReimport.h"
#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/ProjectIntegrityValidatorService.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/document/EditorViewportPicking.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/menu/EditorMenuPlatform.h"
#include "editor/project_model/EditorModelErrors.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneComparisonCaptureDomain{"horo.editor.scene_comparison_capture"};
        const ErrorCodeDescriptor SceneComparisonUnavailable{
            .domain = SceneComparisonCaptureDomain,
            .code = ErrorCode{"scene_comparison_capture.unavailable"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The active scene is unavailable for comparison.",
        };

        [[nodiscard]] std::string ReplaceMessageToken(std::string message, const std::string_view token, const std::string_view value) {
            std::size_t offset = 0;
            while ((offset = message.find(token, offset)) != std::string::npos) {
                message.replace(offset, token.size(), value);
                offset += value.size();
            }
            return message;
        }

        [[nodiscard]] Math::Transform ResolveRuntimeEntityTransform(const Runtime::RuntimeSceneView &runtimeView,
                                                                    const Runtime::RuntimeEntityView &entity) {
            if (!entity.authoredObject.has_value())
                return *entity.localTransform;
            const Result<SceneObjectWorldTransforms> world =
                ResolveSceneObjectWorldTransforms(runtimeView, SceneObjectId{entity.authoredObject->value});
            if (world.HasError())
                return *entity.localTransform;
            if (const Result<Math::Transform> decomposed = Math::TryDecomposeAffineTRS(world.Value().localToWorld); decomposed.HasValue()) {
                return decomposed.Value();
            }
            return *entity.localTransform;
        }

        [[nodiscard]] std::filesystem::path ResolveProjectRoot(const std::filesystem::path &projectRoot) {
            std::error_code error;
            std::filesystem::path resolved = std::filesystem::absolute(projectRoot, error).lexically_normal();
            if (error) {
                error.clear();
                resolved = (std::filesystem::current_path(error) / projectRoot).lexically_normal();
            }
            if (error)
                return resolved;

            const std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, error);
            return error ? resolved : canonical;
        }
    }  // namespace

    EditorWorkspaceController::EditorWorkspaceController(const std::filesystem::path &projectRoot,
                                                         Runtime::RuntimeSceneService &runtimeScene,
                                                         const Assets::AssetRegistrySnapshot &assetRegistry,
                                                         const EditorWorkspaceDependencies &dependencies)
        : m_runtimeScene(runtimeScene), m_assetRegistry(assetRegistry), m_mutableAssetRegistry(dependencies.mutableAssetRegistry),
          m_mutations(dependencies.mutations), m_durableFiles(dependencies.durableFiles), m_importerCatalog(dependencies.importerCatalog),
          m_assetPreviews(dependencies.jobs != nullptr ? std::make_unique<Assets::AssetPreviewService>(*dependencies.jobs) : nullptr),
          m_diagnosticSourceNavigator(dependencies.diagnosticSourceNavigator), m_gameplayBuilds(dependencies.gameplayBuilds),
          m_gameplayBuildEnvironment(dependencies.gameplayBuildEnvironment), m_localization(dependencies.localization),
          m_sceneFileWatch(dependencies.jobs != nullptr ? std::make_unique<SceneFileWatchService>(*dependencies.jobs) : nullptr) {
        if (!m_diagnosticSourceNavigator) {
            m_diagnosticSourceNavigator = [](const DiagnosticSourceRequest &source) {
                const std::filesystem::path path{source.absolutePath};
                return OpenInExternalEditor(path) || RevealInNativeFileManager(path);
            };
        }
        const std::filesystem::path absoluteProjectRoot = ResolveProjectRoot(projectRoot);
        m_viewModel.projectRoot = absoluteProjectRoot.string();
        m_viewModel.assetRegistryRevision = assetRegistry.Revision();
        RebuildContentBrowserProjection(m_viewModel.projectRoot, {});
        if (m_durableFiles != nullptr) {
            ProjectIntegrityValidatorService validator{*m_durableFiles};
            const Result<void> repaired = validator.Repair(absoluteProjectRoot);
            if (repaired.HasError()) {
                LOG_ERROR("editor.project_validator", "Project integrity repair failed for '%s': %s", absoluteProjectRoot.string().c_str(),
                          repaired.ErrorValue().message.c_str());
            }
        }
        m_gameplayRegistry = ProjectGameplayRegistry::Discover(absoluteProjectRoot);
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->Diagnostics())
            LOG_ERROR("editor.gameplay", "Gameplay source '%s' is invalid: %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
        for (const Gameplay::BehaviorRegistration &registration : m_gameplayRegistry->Registry().Registrations())
            m_viewModel.availableBehaviors.push_back(registration.descriptor);
        m_viewModel.panelDockAreas = {{"horo.hierarchy", WorkspaceDockArea::Left},  {"horo.viewport", WorkspaceDockArea::Document},
                                      {"horo.game", WorkspaceDockArea::Document},   {"horo.global_dock", WorkspaceDockArea::Bottom},
                                      {"horo.inspector", WorkspaceDockArea::Right}, {"horo.input_mapping", WorkspaceDockArea::Right}};
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.hierarchy", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.viewport", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.game", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 1}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.global_dock", ActivityBarSlot{ActivityBarRail::Left, 2, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.inspector", ActivityBarSlot{ActivityBarRail::Right, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.input_mapping", ActivityBarSlot{ActivityBarRail::Right, 1, 0}));

        bool loadedProjectScene = false;
        if (const Result<std::optional<LoadedProjectScene>> loaded = LoadProjectDefaultScene(absoluteProjectRoot); loaded.HasError()) {
            m_initializationError = loaded.ErrorValue();
            LOG_ERROR("editor.scene_document", "Default scene load failed: %s", loaded.ErrorValue().message.c_str());
        } else if (loaded.Value().has_value()) {
            LoadedProjectScene projectScene = *loaded.Value();
            if (const Result<void> installed =
                    m_document.LoadSaved(std::move(projectScene.objects), std::move(projectScene.prefabInstances));
                installed.HasError()) {
                m_initializationError = installed.ErrorValue();
                LOG_ERROR("editor.scene_document", "Default scene validation failed: %s", installed.ErrorValue().message.c_str());
            } else {
                m_defaultScenePath = std::move(projectScene.absolutePath);
                m_sceneFingerprint = std::move(projectScene.fingerprint);
                loadedProjectScene = true;
                m_history.Clear();
                LOG_INFO("editor.scene_document", "Loaded default scene '%s'.", m_defaultScenePath->string().c_str());
            }
        }

        if (!loadedProjectScene && !m_initializationError.has_value()) {
            const Math::Quaternion pitch = Math::Quaternion::FromAxisAngle({1.0F, 0.0F, 0.0F}, -0.42F);
            const Math::Quaternion yaw = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 0.55F);
            const Result<SceneCommandResult> created = m_documentCommands.Execute(CreateSceneObjectCommand{
                .name = "Box",
                .localTransform = Math::Transform{.rotation = pitch * yaw},
                .primitiveMesh = PrimitiveMeshDescriptor{},
            });
            if (created.HasError()) {
                LOG_ERROR("editor.scene_document", "Bootstrap scene creation failed: %s", created.ErrorValue().message.c_str());
            } else {
                static_cast<void>(m_document.MarkSaved(m_document.Revision(), m_document.State()));
                m_history.Clear();
            }
        }
        if (m_defaultScenePath.has_value()) {
            const Result<std::optional<ProjectSceneRecoveryRecord>> recovery =
                InspectProjectSceneRecovery(absoluteProjectRoot, *m_defaultScenePath);
            if (recovery.HasError()) {
                LOG_ERROR("editor.scene_recovery", "Recovery inspection failed: %s", recovery.ErrorValue().message.c_str());
            } else if (recovery.Value().has_value()) {
                m_viewModel.recoveryAvailable = true;
                LOG_WARN("editor.scene_recovery", "Validated recovery is available for '%s'; canonical scene was not modified.",
                         m_defaultScenePath->string().c_str());
            }
        }
        RefreshSceneProjections();
    }

    /** @copydoc EditorWorkspaceController::UpdateExternalSceneWatch */
    void EditorWorkspaceController::UpdateExternalSceneWatch(const float elapsedSeconds) {
        if (m_sceneFileWatch == nullptr || !m_defaultScenePath.has_value() || !m_sceneFingerprint.has_value())
            return;

        for (SceneFileWatchUpdate &update : m_sceneFileWatch->DrainUpdates()) {
            if (update.error.has_value()) {
                if (!m_sceneFileWatchErrorPresented) {
                    LOG_WARN("editor.scene_document", "Background scene inspection failed for '%s': %s",
                             m_defaultScenePath->string().c_str(), update.error->message.c_str());
                    m_sceneFileWatchErrorPresented = true;
                }
                continue;
            }
            if (!update.fingerprint.has_value())
                continue;
            m_sceneFileWatchErrorPresented = false;
            const bool conflict = *update.fingerprint != *m_sceneFingerprint;
            if (conflict && !m_viewModel.sceneExternalConflict) {
                LOG_WARN("editor.scene_document", "Canonical scene changed outside this document session: '%s'.",
                         m_defaultScenePath->string().c_str());
            }
            m_viewModel.sceneExternalConflict = conflict;
        }

        if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F || m_sceneFileWatch->HasPendingInspection())
            return;
        m_sceneFileWatchElapsedSeconds += elapsedSeconds;
        if (m_sceneFileWatchElapsedSeconds < 1.0F)
            return;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        const Result<std::uint64_t> requested =
            m_sceneFileWatch->Request(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
        if (requested.HasError() && !m_sceneFileWatchErrorPresented) {
            LOG_WARN("editor.scene_document", "Background scene inspection could not be scheduled: %s",
                     requested.ErrorValue().message.c_str());
            m_sceneFileWatchErrorPresented = true;
        }
    }

    /** @copydoc EditorWorkspaceController::CaptureExternalSceneComparison */
    Result<SceneDocumentComparisonRequest> EditorWorkspaceController::CaptureExternalSceneComparison() const {
        if (!m_defaultScenePath.has_value())
            return Result<SceneDocumentComparisonRequest>::Failure(MakeError(SceneComparisonUnavailable));
        return Result<SceneDocumentComparisonRequest>::Success({
            .absoluteProjectRoot = std::filesystem::path{m_viewModel.projectRoot},
            .absoluteScenePath = *m_defaultScenePath,
            .document = m_document.Snapshot(),
        });
    }

    void EditorWorkspaceController::UpdateFps(const float fps) {
        m_viewModel.fps = fps;
    }

    void EditorWorkspaceController::UpdateGameplaySources(const float elapsedSeconds) {
        UpdateGameplayBuild(elapsedSeconds);
        if (m_nativeGameplayReloadQuarantined || m_gameplayRegistry == nullptr || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F)
            return;
        m_gameplaySourceWatchElapsedSeconds += elapsedSeconds;
        if (m_gameplaySourceWatchElapsedSeconds < 0.5F)
            return;
        m_gameplaySourceWatchElapsedSeconds = 0.0F;
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->ReloadChangedLuaSources())
            LOG_ERROR("editor.gameplay", "Lua reload kept the last working revision for '%s': %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
        if (!m_gameplayRegistry->ConsumeNativeArtifactChange())
            return;
        if (m_playSession.IsActive()) {
            m_nativeGameplayReloadPending = true;
            return;
        }
        std::unique_ptr<ProjectGameplayRegistry> candidate = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (candidate->HasBlockingDiagnostics()) {
            for (const ProjectGameplayDiagnostic &diagnostic : candidate->Diagnostics())
                LOG_ERROR("editor.gameplay", "Native reload kept the last working module for '%s': %s", diagnostic.source.string().c_str(),
                          diagnostic.error.message.c_str());
            return;
        }
        m_gameplayRegistry = std::move(candidate);
        RefreshAvailableBehaviorProjection();
    }

    /** @copydoc EditorWorkspaceController::UpdateAutosave */
    void EditorWorkspaceController::UpdateAutosave(const float elapsedSeconds, const int intervalMinutes) {
        if (intervalMinutes <= 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F || !m_document.IsDirty() ||
            m_viewModel.recoveryAvailable || m_autosaveSuppressedForDiscard) {
            if (!m_document.IsDirty())
                m_autosaveElapsedSeconds = 0.0F;
            return;
        }
        if (m_document.State() == m_lastAutosavedState)
            return;
        if (m_autosaveRetryDelaySeconds > 0.0F) {
            m_autosaveRetryDelaySeconds = std::max(0.0F, m_autosaveRetryDelaySeconds - elapsedSeconds);
            if (m_autosaveRetryDelaySeconds > 0.0F)
                return;
        }

        m_autosaveElapsedSeconds += elapsedSeconds;
        const float intervalSeconds = static_cast<float>(intervalMinutes) * 60.0F;
        if (m_autosaveElapsedSeconds >= intervalSeconds)
            WriteAutosaveRecovery();
    }

    /** @copydoc EditorWorkspaceController::FlushAutosave */
    void EditorWorkspaceController::FlushAutosave() {
        if (m_document.IsDirty() && !m_viewModel.recoveryAvailable && !m_autosaveSuppressedForDiscard &&
            m_document.State() != m_lastAutosavedState)
            WriteAutosaveRecovery();
    }

    /** @copydoc EditorWorkspaceController::WriteAutosaveRecovery */
    void EditorWorkspaceController::WriteAutosaveRecovery() {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr)
            return;
        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        if (const Result<void> written =
                WriteProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath, snapshot,
                                          m_document.SavedRevision(), m_document.SavedState(), *m_mutations, *m_durableFiles);
            written.HasError()) {
            m_autosaveRetryDelaySeconds = 30.0F;
            LOG_ERROR("editor.scene_recovery", "Autosave recovery write failed: %s", written.ErrorValue().message.c_str());
            return;
        }
        m_lastAutosavedState = snapshot.state;
        m_autosaveElapsedSeconds = 0.0F;
        m_autosaveRetryDelaySeconds = 0.0F;
        LOG_INFO("editor.scene_recovery", "Autosaved recovery revision %llu for '%s'.",
                 static_cast<unsigned long long>(snapshot.revision.value), m_defaultScenePath->string().c_str());
    }

    /** @copydoc EditorWorkspaceController::SaveScene */
    void EditorWorkspaceController::SaveScene(const bool overwriteConflict) {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr) {
            LOG_ERROR("editor.scene_document", "Save rejected because the project scene path or "
                                               "durable writer services are unavailable.");
            return;
        }

        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        if (!m_sceneFingerprint.has_value()) {
            LOG_ERROR("editor.scene_document", "Save rejected because the canonical scene identity is unavailable.");
            return;
        }
        const Result<ProjectSceneSaveResult> saved =
            SaveProjectScene(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath, snapshot, *m_sceneFingerprint,
                             overwriteConflict, *m_mutations, *m_durableFiles);
        if (saved.HasError()) {
            LOG_ERROR("editor.scene_document", "Scene save failed for '%s': %s", m_defaultScenePath->string().c_str(),
                      saved.ErrorValue().message.c_str());
            return;
        }
        if (saved.Value().status == ProjectSceneSaveStatus::Conflict) {
            m_viewModel.sceneExternalConflict = true;
            LOG_WARN("editor.scene_document", "Save paused because '%s' changed outside this document session.",
                     m_defaultScenePath->string().c_str());
            return;
        }

        if (const Result<void> marked = m_document.MarkSaved(snapshot.revision, snapshot.state); marked.HasError()) {
            LOG_ERROR("editor.scene_document", "Saved scene state could not be acknowledged: %s", marked.ErrorValue().message.c_str());
            return;
        }
        m_sceneFingerprint = saved.Value().fingerprint;
        if (m_sceneFileWatch != nullptr)
            m_sceneFileWatch->Reset();
        m_viewModel.sceneExternalConflict = false;
        m_sceneFileWatchErrorPresented = false;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        if (const Result<void> recoveryDiscarded =
                DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
            recoveryDiscarded.HasError()) {
            LOG_WARN("editor.scene_recovery", "Saved scene but could not clean recovery state: %s",
                     recoveryDiscarded.ErrorValue().message.c_str());
        } else {
            m_viewModel.recoveryAvailable = false;
            m_lastAutosavedState = {};
        }
        m_autosaveSuppressedForDiscard = false;
        m_dataBus.Publish(SceneDocumentChangedEvent{m_document.Revision(),
                                                    m_document.State(),
                                                    DocumentChangeKind::SaveStateChanged,
                                                    m_document.IsDirty(),
                                                    {}});
        RefreshSceneProjections();
        LOG_INFO("editor.scene_document", "Saved scene revision %llu to '%s'.", static_cast<unsigned long long>(snapshot.revision.value),
                 m_defaultScenePath->string().c_str());
    }

    /** @copydoc EditorWorkspaceController::SaveSceneToPath */
    void EditorWorkspaceController::SaveSceneToPath(const std::filesystem::path &absolutePath, const bool copyOnly) {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because the active scene or "
                                               "durable writer services are unavailable.");
            return;
        }

        std::error_code canonicalError;
        const std::filesystem::path destination = std::filesystem::weakly_canonical(absolutePath, canonicalError);
        if (canonicalError) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' could not be normalized: %s",
                      absolutePath.string().c_str(), canonicalError.message().c_str());
            return;
        }
        const std::filesystem::path activePath = m_defaultScenePath->lexically_normal();
        if (!destination.is_absolute()) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' is not absolute.", destination.string().c_str());
            return;
        }
        if (copyOnly && destination == activePath) {
            LOG_ERROR("editor.scene_document", "Save Copy As rejected because the destination is the active scene path '%s'.",
                      activePath.string().c_str());
            return;
        }
        if (!copyOnly && destination == activePath) {
            SaveScene(true);
            return;
        }

        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        auto saved = SaveProjectSceneToPath(std::filesystem::path{m_viewModel.projectRoot}, destination, snapshot, true, *m_mutations,
                                            *m_durableFiles);
        if (saved.HasError()) {
            LOG_ERROR("editor.scene_document", "%s failed for '%s': %s", copyOnly ? "Save Copy As" : "Save As",
                      destination.string().c_str(), saved.ErrorValue().message.c_str());
            return;
        }
        if (saved.Value().status != ProjectSceneDestinationSaveStatus::Saved) {
            LOG_WARN("editor.scene_document", "%s paused because destination '%s' changed during the save.",
                     copyOnly ? "Save Copy As" : "Save As", destination.string().c_str());
            return;
        }

        if (copyOnly) {
            LOG_INFO("editor.scene_document", "Saved scene copy revision %llu to '%s'.",
                     static_cast<unsigned long long>(snapshot.revision.value), destination.string().c_str());
            return;
        }

        if (const Result<void> marked = m_document.MarkSaved(snapshot.revision, snapshot.state); marked.HasError()) {
            LOG_ERROR("editor.scene_document",
                      "Saved As destination is durable but the active document state could not be "
                      "acknowledged: %s",
                      marked.ErrorValue().message.c_str());
            return;
        }

        m_defaultScenePath = destination;
        m_sceneFingerprint = saved.Value().fingerprint;
        if (m_sceneFileWatch != nullptr)
            m_sceneFileWatch->Reset();
        m_viewModel.sceneExternalConflict = false;
        m_sceneFileWatchErrorPresented = false;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        if (const Result<void> recoveryDiscarded =
                DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
            recoveryDiscarded.HasError()) {
            LOG_WARN("editor.scene_recovery", "Saved scene to a new path but could not clean recovery state: %s",
                     recoveryDiscarded.ErrorValue().message.c_str());
        } else {
            m_viewModel.recoveryAvailable = false;
            m_lastAutosavedState = {};
        }
        m_autosaveSuppressedForDiscard = false;
        m_dataBus.Publish(SceneDocumentChangedEvent{m_document.Revision(),
                                                    m_document.State(),
                                                    DocumentChangeKind::SaveStateChanged,
                                                    m_document.IsDirty(),
                                                    {}});
        RefreshSceneProjections();
        LOG_INFO("editor.scene_document", "Saved scene revision %llu as '%s'; active document identity was updated.",
                 static_cast<unsigned long long>(snapshot.revision.value), destination.string().c_str());
    }

    /** @copydoc EditorWorkspaceController::ReloadExternalScene */
    void EditorWorkspaceController::ReloadExternalScene() {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr)
            return;

        if (const Result<LoadedProjectScene> loaded = LoadProjectScene(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
            loaded.HasError() || !loaded.Value().existed) {
            LOG_ERROR("editor.scene_document", "External scene reload failed because the canonical document is unavailable.");
            return;
        } else {
            LoadedProjectScene external = loaded.Value();
            SceneDocument validatedExternal;
            if (const Result<void> validated = validatedExternal.LoadSaved(external.objects, external.prefabInstances);
                validated.HasError()) {
                LOG_ERROR("editor.scene_document", "External scene validation failed: %s", validated.ErrorValue().message.c_str());
                return;
            }
            if (const Result<void> recoveryDiscarded =
                    DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
                recoveryDiscarded.HasError()) {
                LOG_ERROR("editor.scene_recovery", "External reload could not discard superseded recovery state: %s",
                          recoveryDiscarded.ErrorValue().message.c_str());
                return;
            }
            if (const Result<void> installed = m_document.LoadSaved(std::move(external.objects), std::move(external.prefabInstances));
                installed.HasError()) {
                LOG_ERROR("editor.scene_document", "External scene validation failed: %s", installed.ErrorValue().message.c_str());
                return;
            }

            m_sceneFingerprint = std::move(external.fingerprint);
        }
        if (m_sceneFileWatch != nullptr)
            m_sceneFileWatch->Reset();
        m_history.Clear();
        m_selection.Clear();
        m_deferredRuntimeSnapshot.reset();
        m_activeRuntimeRevision = {};
        m_queuedDefinitionRevision = {};
        m_viewModel.sceneExternalConflict = false;
        m_sceneFileWatchErrorPresented = false;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = {};
        m_autosaveElapsedSeconds = 0.0F;
        m_autosaveRetryDelaySeconds = 0.0F;
        m_autosaveSuppressedForDiscard = false;
        RefreshSceneProjections();
        LOG_INFO("editor.scene_document", "Reloaded externally changed scene '%s'.", m_defaultScenePath->string().c_str());
    }

    /** @copydoc EditorWorkspaceController::RestoreSceneRecovery */
    void EditorWorkspaceController::RestoreSceneRecovery() {
        if (!m_defaultScenePath.has_value())
            return;
        Result<std::optional<ProjectSceneRecoveryRecord>> recovery =
            InspectProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
        if (recovery.HasError() || !recovery.Value().has_value()) {
            LOG_ERROR("editor.scene_recovery", "Recovery restore failed because no valid record is available.");
            return;
        }
        std::optional<ProjectSceneRecoveryRecord> recoveryRecord = std::move(recovery).Value();
        if (const Result<void> restored =
                m_document.LoadRecovered(std::move(recoveryRecord->objects), std::move(recoveryRecord->prefabInstances));
            restored.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery restore validation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_history.Clear();
        m_selection.Clear();
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = m_document.State();
        m_autosaveSuppressedForDiscard = false;
        RefreshSceneProjections();
        LOG_INFO("editor.scene_recovery", "Recovery restored into a new dirty document session.");
    }

    /** @copydoc EditorWorkspaceController::DiscardSceneRecovery */
    void EditorWorkspaceController::DiscardSceneRecovery() {
        if (m_mutations == nullptr || m_durableFiles == nullptr)
            return;
        if (const Result<void> discarded =
                DiscardProjectSceneRecovery(std::filesystem::path{m_viewModel.projectRoot}, *m_mutations, *m_durableFiles);
            discarded.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery discard failed: %s", discarded.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.recoveryAvailable = false;
        m_lastAutosavedState = {};
        m_autosaveSuppressedForDiscard = true;
        LOG_INFO("editor.scene_recovery", "Recovery state discarded explicitly.");
    }

    /** @copydoc EditorWorkspaceController::RefreshAssets */

    /** @copydoc EditorWorkspaceController::UpdateContentBrowser */

    void EditorWorkspaceController::HandleCreatePrimitive(const Runtime::PrimitiveId primitive, const std::optional<SceneObjectId> parent) {
        Result<SceneCommandResult> result = m_createSceneObject.Execute(PrimitiveCreationRequest{primitive, parent});
        if (result.HasError()) {
            HandleDocumentCommandResult(result, "Create object");
            return;
        }
        const SceneObjectId created = result.Value().object;
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Create object");
        if (committed) {
            m_viewModel.hierarchyRevealObject = created;
            m_viewModel.hierarchyRevealRevision = m_document.Revision();
            if (const Result<void> selected = m_selection.SetObjects({created}, created); selected.HasError()) {
                LOG_ERROR("editor.selection", "Select created object failed: %s", selected.ErrorValue().message.c_str());
            }
            RefreshSelectionProjection();
        }
    }

    bool EditorWorkspaceController::ApplyAssetViewportPlacement(const AssetSceneDropRequest &request, const Math::Aabb &localBounds,
                                                                Math::Transform &localTransform) const {
        if (request.target != AssetSceneDropTarget::Viewport)
            return true;

        const Result<AssetViewportPlacement> placement = ResolveAssetViewportPlacement(AssetViewportPlacementRequest{
            .scene = m_viewportScene,
            .normalizedX = request.normalizedX,
            .normalizedY = request.normalizedY,
            .aspect = request.aspect,
            .depthRange = request.depthRange,
            .localBounds = localBounds,
        });
        if (placement.HasError()) {
            m_notifications.Publish("asset", NotificationSeverity::Error, placement.ErrorValue().message,
                                    Localized("workspace.asset_drop.place_failed_title", "Asset could not be placed"),
                                    "asset_drop_placement_failed");
            return false;
        }
        localTransform.translation = placement.Value().worldPosition;
        if (!request.parent.has_value())
            return true;

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active.has_value()) {
            m_notifications.Publish("asset", NotificationSeverity::Warning,
                                    Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_parent_runtime_missing");
            return false;
        }
        const Result<SceneObjectWorldTransforms> parentWorld = ResolveSceneObjectWorldTransforms(*active, *request.parent);
        const Result<Math::Mat4> parentInverse = parentWorld.HasValue() ? Math::TryInverseAffine(parentWorld.Value().localToWorld)
                                                                        : Result<Math::Mat4>::Failure(parentWorld.ErrorValue());
        if (parentInverse.HasError()) {
            m_notifications.Publish("asset", NotificationSeverity::Warning,
                                    Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_parent_transform_invalid");
            return false;
        }
        localTransform.translation = Math::TransformAffinePoint(parentInverse.Value(), placement.Value().worldPosition);
        return true;
    }

    void EditorWorkspaceController::HandleInstantiateAsset(const AssetSceneDropRequest &request) {
        const auto parsedId = Assets::AssetId::Parse(request.assetId);
        if (parsedId.HasError() || request.documentRevision != m_document.Revision()) {
            m_notifications.Publish("asset", NotificationSeverity::Warning,
                                    Localized("workspace.asset_drop.stale",
                                              "The asset drop was cancelled because the scene or drag reference changed."),
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_stale");
            return;
        }
        const Assets::AssetRecord *record = m_assetRegistry.Find(parsedId.Value());
        if (record == nullptr || record->type.Value() != request.assetType) {
            m_notifications.Publish("asset", NotificationSeverity::Error,
                                    Localized("workspace.asset_drop.missing", "The dragged asset is no longer registered in this project."),
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_missing");
            return;
        }
        if (!CanInstantiateAssetType(record->type.Value())) {
            m_notifications.Publish("asset", NotificationSeverity::Info,
                                    Localized("workspace.asset_drop.unsupported",
                                              "This asset type cannot be instantiated as a scene object."),
                                    Localized("workspace.asset_drop.unsupported_title", "Unsupported asset"),
                                    std::format("asset_drop_unsupported_{}", record->type.Value()));
            return;
        }
        if (request.parent.has_value() && !m_document.Contains(*request.parent)) {
            m_notifications.Publish("asset", NotificationSeverity::Warning,
                                    Localized("workspace.asset_drop.parent_missing", "The hierarchy target no longer exists."),
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_parent_missing");
            return;
        }

        const std::filesystem::path source =
            (std::filesystem::path{m_viewModel.projectRoot} / record->sourcePath.String()).lexically_normal();
        auto loaded = m_assetMeshCache.Load(record->id, source);
        if (loaded.HasError()) {
            m_notifications.Publish("asset", NotificationSeverity::Error, loaded.ErrorValue().message,
                                    Localized("workspace.asset_drop.load_failed_title", "Asset could not be loaded"),
                                    std::format("asset_drop_load_failed_{}", request.assetId));
            return;
        }

        Math::Transform localTransform;
        if (!ApplyAssetViewportPlacement(request, loaded.Value().mesh->localBounds, localTransform))
            return;

        std::string baseName = source.stem().string();
        if (baseName.empty())
            baseName = "Mesh";
        Result<SceneCommandResult> result =
            m_instantiateSceneAsset.Execute(AssetInstantiationRequest{record->id, std::move(baseName), request.parent, localTransform});
        if (result.HasError()) {
            const std::string message = result.ErrorValue().message;
            HandleDocumentCommandResult(result, "Instantiate asset");
            m_notifications.Publish("asset", NotificationSeverity::Error, message,
                                    Localized("workspace.asset_drop.not_added", "Asset not added"), "asset_drop_command_failed");
            return;
        }
        const SceneObjectId created = result.Value().object;
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Instantiate asset");
        if (!committed)
            return;
        m_viewModel.hierarchyRevealObject = created;
        m_viewModel.hierarchyRevealRevision = m_document.Revision();
        if (const Result<void> selected = m_selection.SetObjects({created}, created); selected.HasError())
            LOG_ERROR("editor.selection", "Select instantiated asset failed: %s", selected.ErrorValue().message.c_str());
        RefreshSelectionProjection();
        m_notifications.Publish("asset", NotificationSeverity::Success,
                                Localized("workspace.asset_drop.success", "Asset added to the scene."),
                                Localized("workspace.asset_drop.success_title", "Asset added"), "asset_drop_success");
    }

    std::string EditorWorkspaceController::Localized(const std::string_view key, const std::string_view fallback) const {
        return m_localization != nullptr ? m_localization->Get("editor", key) : std::string{fallback};
    }

    void EditorWorkspaceController::LoadDocumentAssetMeshes() {
        for (const SceneObjectSnapshot &object : m_document.Objects()) {
            if (!object.meshAsset.has_value())
                continue;
            const Assets::AssetRecord *record = m_assetRegistry.Find(*object.meshAsset);
            if (record == nullptr || record->type.Value() != "core.mesh")
                continue;
            const std::filesystem::path source =
                (std::filesystem::path{m_viewModel.projectRoot} / record->sourcePath.String()).lexically_normal();
            const auto loaded = m_assetMeshCache.Load(record->id, source);
            if (loaded.HasError())
                LOG_WARN("editor.asset", "Unable to load scene mesh asset: %s", loaded.ErrorValue().message.c_str());
        }
    }

    void EditorWorkspaceController::HandleDuplicateObject(const SceneObjectId object) {
        const auto source = std::ranges::find(m_viewModel.objects, object, &SceneObject::id);
        if (source != m_viewModel.objects.end()) {
            HandleDocumentCommandResult(m_documentCommands.Execute(DuplicateSceneObjectCommand{source->id, source->name + " Copy"}),
                                        "Duplicate object");
        }
    }

    void EditorWorkspaceController::HandleDeleteObject(const SceneObjectId object) {
        HandleDeleteSelectedObjects({object});
    }

    void EditorWorkspaceController::HandleDeleteSelectedObjects(const std::vector<SceneObjectId> &objects) {
        std::vector<SceneObjectId> requested;
        requested.reserve(objects.size());
        for (const SceneObjectId object : objects) {
            if (object.IsValid() && std::ranges::find(requested, object) == requested.end())
                requested.push_back(object);
        }

        std::vector<SceneObjectId> existing;
        existing.reserve(requested.size());
        std::string singleName;
        const std::span<const SceneObjectSnapshot> documentObjects = m_document.Objects();
        for (const SceneObjectId object : requested) {
            const auto found = std::ranges::find(documentObjects, object, &SceneObjectSnapshot::id);
            if (found == documentObjects.end())
                continue;
            existing.push_back(object);
            if (existing.size() == 1)
                singleName = found->name;
        }
        const std::size_t skipped = requested.size() - existing.size();
        if (existing.empty()) {
            m_notifications.Publish("scene", NotificationSeverity::Warning,
                                    Localized("workspace.hierarchy.delete_blocked", "Selected objects cannot be deleted."),
                                    Localized("workspace.hierarchy.delete_failed_title", "Delete failed"), "scene_delete_blocked");
            return;
        }

        Result<SceneCommandResult> result = m_documentCommands.Execute(DeleteSceneObjectsCommand{existing});
        if (result.HasError()) {
            HandleDocumentCommandResult(result, "Delete selected objects");
            m_notifications.Publish("scene", NotificationSeverity::Error,
                                    Localized("workspace.hierarchy.delete_blocked", "Selected objects cannot be deleted."),
                                    Localized("workspace.hierarchy.delete_failed_title", "Delete failed"), "scene_delete_failed");
            return;
        }
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Delete selected objects");
        if (!committed)
            return;

        std::string message;
        NotificationSeverity severity = NotificationSeverity::Success;
        if (skipped > 0) {
            message =
                Localized("workspace.hierarchy.delete_partial", "{deletedCount} objects deleted, {skippedCount} could not be deleted.");
            message = ReplaceMessageToken(std::move(message), "{deletedCount}", std::to_string(existing.size()));
            message = ReplaceMessageToken(std::move(message), "{skippedCount}", std::to_string(skipped));
            severity = NotificationSeverity::Warning;
        } else if (existing.size() == 1) {
            message = Localized("workspace.hierarchy.delete_success_single", "\"{name}\" deleted.");
            message = ReplaceMessageToken(std::move(message), "{name}", singleName);
        } else {
            message = Localized("workspace.hierarchy.delete_success_multiple", "{count} objects deleted.");
            message = ReplaceMessageToken(std::move(message), "{count}", std::to_string(existing.size()));
        }
        m_notifications.Publish("scene", severity, std::move(message),
                                Localized("workspace.hierarchy.delete_success_title", "Objects deleted"), "scene_delete_result");
    }

    void EditorWorkspaceController::HandleDocumentCommandResult(const Result<SceneCommandResult> &result, const char *operation) {
        if (result.HasError()) {
            LOG_ERROR("editor.scene_document", "%s failed: %s", operation, result.ErrorValue().message.c_str());
            if (result.ErrorValue().code.Value() == SceneDocumentErrors::ObjectLocked.code.Value()) {
                m_notifications.Publish("scene", NotificationSeverity::Warning,
                                        Localized("workspace.hierarchy.locked_edit_blocked",
                                                  "Unlock the object or its parent before editing it."),
                                        Localized("workspace.hierarchy.locked_edit_blocked_title", "Object is locked"),
                                        "scene_object_locked");
            }
            CancelObjectTransformPreview();
            CancelLightComponentPreview();
            return;
        }
        const SceneCommandResult &committed = result.Value();
        if (!committed.committed) {
            CancelObjectTransformPreview();
            CancelLightComponentPreview();
            return;
        }
        m_viewport.ClearTransformPreview();
        m_viewport.ClearLightPreview();
        m_dataBus.Publish(SceneDocumentChangedEvent{committed.revision, committed.state, committed.kind, m_document.IsDirty(),
                                                    committed.affectedObjects});
        m_selection.Reconcile();
        RefreshSceneProjections();
    }

    void EditorWorkspaceController::PreviewObjectTransform(const SceneObjectId object, const Math::Transform &transform) {
        const SceneObjectTransformUpdate update{object, transform};
        PreviewObjectTransforms(std::span{&update, 1});
    }

    void EditorWorkspaceController::PreviewObjectTransforms(const std::span<const SceneObjectTransformUpdate> updates) {
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId() || updates.empty())
            return;
        if (std::ranges::any_of(updates, [&](const SceneObjectTransformUpdate &update) {
            const std::optional<ResolvedSceneObjectEditorState> state = ResolveSceneObjectEditorState(m_document.Objects(), update.object);
            return !state.has_value() || state->effectivelyLocked;
        }))
            return;

        std::vector<SceneObjectTransformPreview> previews;
        previews.reserve(updates.size());
        for (const SceneObjectTransformUpdate &update : updates)
            previews.emplace_back(update.object, update.localTransform);

        const std::vector<SceneObjectTransformPreview> previousPreviews = m_viewport.Current().transformPreviews;
        if (Result<void> applied = ApplyEditorViewportTransformPreview(*active, previews, m_viewportScene); applied.HasError()) {
            LOG_ERROR("editor.viewport", "Transform preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        if (Result<void> committed = m_viewport.SetTransformPreviews(previews); committed.HasError()) {
            if (const Result<void> restored = ApplyEditorViewportTransformPreview(*active, previousPreviews, m_viewportScene);
                restored.HasError()) {
                LOG_ERROR("editor.viewport", "Transform preview rollback failed: %s", restored.ErrorValue().message.c_str());
            }
            LOG_ERROR("editor.viewport", "Transform preview state failed: %s", committed.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        if (m_viewModel.primarySelection.has_value()) {
            const auto instance = std::ranges::find(m_viewportScene.instanceObjects, *m_viewModel.primarySelection);
            if (instance != m_viewportScene.instanceObjects.end()) {
                const auto index = static_cast<std::size_t>(std::distance(m_viewportScene.instanceObjects.begin(), instance));
                m_viewModel.primarySelectionPreviewWorldTransform = m_viewportScene.instances[index].localToWorld;
            }
        }
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::CancelObjectTransformPreview() {
        if (m_viewport.Current().transformPreviews.empty()) {
            return;
        }
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;
        if (const Result<void> restored =
                ApplyEditorViewportTransformPreview(*active, std::span<const SceneObjectTransformPreview>{}, m_viewportScene);
            restored.HasError()) {
            LOG_ERROR("editor.viewport", "Transform preview cancellation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_viewport.ClearTransformPreview();
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::PreviewLightComponent(const SceneObjectId object, const Runtime::LightComponent &light) {
        if (const std::optional<ResolvedSceneObjectEditorState> editorState = ResolveSceneObjectEditorState(m_document.Objects(), object);
            !editorState.has_value() || editorState->effectivelyLocked)
            return;
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;

        const SceneObjectLightPreview preview{object, light};
        const std::optional<SceneObjectLightPreview> previousPreview = m_viewport.Current().lightPreview;
        if (Result<void> applied = ApplyEditorViewportLightPreview(*active, &preview, m_viewportScene); applied.HasError()) {
            LOG_ERROR("editor.viewport", "Light preview failed: %s", applied.ErrorValue().message.c_str());
            return;
        }
        if (Result<void> committed = m_viewport.SetLightPreview(preview); committed.HasError()) {
            const SceneObjectLightPreview *previous = previousPreview.has_value() ? &*previousPreview : nullptr;
            if (const Result<void> restored = ApplyEditorViewportLightPreview(*active, previous, m_viewportScene); restored.HasError())
                LOG_ERROR("editor.viewport", "Light preview rollback failed: %s", restored.ErrorValue().message.c_str());
            LOG_ERROR("editor.viewport", "Light preview state failed: %s", committed.ErrorValue().message.c_str());
        }
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::CancelLightComponentPreview() {
        if (!m_viewport.Current().lightPreview.has_value())
            return;
        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || m_viewportScene.runtimeSceneId != active->RuntimeId())
            return;
        if (const Result<void> restored = ApplyEditorViewportLightPreview(*active, nullptr, m_viewportScene); restored.HasError()) {
            LOG_ERROR("editor.viewport", "Light preview cancellation failed: %s", restored.ErrorValue().message.c_str());
            return;
        }
        m_viewport.ClearLightPreview();
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::RefreshSceneProjections() {
        const SceneDocumentSnapshot documentSnapshot = m_document.Snapshot();
        m_viewModel.documentRevision = documentSnapshot.revision;
        m_viewModel.objects.clear();
        m_viewModel.objects.reserve(documentSnapshot.objects.size());
        for (const SceneObjectSnapshot &object : documentSnapshot.objects) {
            int componentCount = 0;
            using enum SceneObjectKind;
            SceneObjectKind singleComponentKind = GameObject;

            if (object.primitiveMesh.has_value() || object.meshAsset.has_value()) {
                componentCount++;
                singleComponentKind = Mesh;
            }
            if (object.components.camera.has_value()) {
                componentCount++;
                singleComponentKind = Camera;
            }
            if (object.components.light.has_value()) {
                componentCount++;
                singleComponentKind = Light;
            }
            if (object.components.triggerVolume.has_value()) {
                componentCount++;
                singleComponentKind = TriggerVolume;
            }
            if (object.components.audioSource.has_value()) {
                componentCount++;
                singleComponentKind = AudioSource;
            }

            SceneObjectKind kind = (componentCount == 1) ? singleComponentKind : GameObject;
            const ResolvedSceneObjectEditorState editorState = *ResolveSceneObjectEditorState(documentSnapshot.objects, object.id);
            m_viewModel.objects.push_back(SceneObject{.id = object.id,
                                                      .parent = object.parent,
                                                      .name = object.name,
                                                      .kind = kind,
                                                      .localTransform = object.localTransform,
                                                      .components = object.components,
                                                      .editorState = object.editorState,
                                                      .effectivelyVisible = editorState.effectivelyVisible,
                                                      .effectivelyLocked = editorState.effectivelyLocked,
                                                      .hiddenByParent = editorState.hiddenByParent,
                                                      .lockedByParent = editorState.lockedByParent});
        }
        m_viewModel.isDirty = m_document.IsDirty();
        m_viewModel.canUndo = m_history.CanUndo();
        m_viewModel.canRedo = m_history.CanRedo();
        QueueRuntimeScene(documentSnapshot);
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::QueueRuntimeScene(SceneDocumentSnapshot snapshot) {
        if (snapshot.state.value == m_activeRuntimeRevision.value || snapshot.state.value == m_queuedDefinitionRevision.value ||
            (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state == snapshot.state))
            return;

        Result<Runtime::RuntimeSceneDefinition> definition = ConvertSceneDocumentToRuntime(snapshot, m_previewSceneId);
        if (definition.HasError()) {
            LOG_ERROR("editor.runtime_scene", "Scene conversion failed: %s", definition.ErrorValue().message.c_str());
            return;
        }
        if (const Result<void> queued = m_runtimeScene.QueuePreparation(definition.Value()); queued.HasError()) {
            m_deferredRuntimeSnapshot = std::move(snapshot);
            return;
        }
        m_queuedRuntimeRevision = snapshot.revision;
        m_queuedDefinitionRevision = Runtime::SceneDefinitionRevision{snapshot.state.value};
    }

    void EditorWorkspaceController::SynchronizeRuntimeScenePreview() {
        if (m_playSession.IsActive()) {
            ExtractPlayViewportScene();
            return;
        }
        if (std::optional<Error> operationError = m_runtimeScene.TakeOperationError())
            LOG_ERROR("editor.runtime_scene", "Runtime scene operation failed: %s", operationError->message.c_str());

        const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
        if (!active || active->DefinitionRevision() == m_activeRuntimeRevision)
            return;

        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        Result<EditorViewportSceneSnapshot> extracted =
            ExtractEditorViewportScene(*active, m_queuedRuntimeRevision, m_viewport.Current().camera, m_primitiveMeshCache, &document,
                                       &m_assetMeshCache);
        if (extracted.HasError()) {
            LOG_ERROR("editor.viewport", "Runtime scene extraction failed: %s", extracted.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene = std::move(extracted).Value();
        ++m_viewportSceneRevision;
        if (!m_viewport.Current().transformPreviews.empty()) {
            const Result<void> reapplied =
                ApplyEditorViewportTransformPreview(*active, m_viewport.Current().transformPreviews, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Transform preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        if (m_viewport.Current().lightPreview.has_value()) {
            const Result<void> reapplied = ApplyEditorViewportLightPreview(*active, &*m_viewport.Current().lightPreview, m_viewportScene);
            if (reapplied.HasError())
                LOG_ERROR("editor.viewport", "Light preview reapply failed: %s", reapplied.ErrorValue().message.c_str());
        }
        RefreshViewportLightProjection();
        m_activeRuntimeRevision = active->DefinitionRevision();
        if (m_queuedDefinitionRevision == m_activeRuntimeRevision)
            m_queuedDefinitionRevision = {};
        m_selection.Reconcile();
        RefreshSelectionProjection();

        if (m_deferredRuntimeSnapshot && m_deferredRuntimeSnapshot->state.value != m_activeRuntimeRevision.value) {
            SceneDocumentSnapshot deferred = std::move(*m_deferredRuntimeSnapshot);
            m_deferredRuntimeSnapshot.reset();
            QueueRuntimeScene(std::move(deferred));
        }
    }

    Application::GameplayBuildRequest EditorWorkspaceController::MakeGameplayBuildRequest() const {
        return Application::GameplayBuildRequest{
            .projectRoot = m_viewModel.projectRoot,
            .environment = m_gameplayBuildEnvironment,
        };
    }

    bool EditorWorkspaceController::HasNativeGameplaySources() const {
        const std::filesystem::path sourceRoot = std::filesystem::path{m_viewModel.projectRoot} / "source" / "gameplay";
        std::error_code error;
        if (!std::filesystem::is_directory(sourceRoot, error))
            return false;
        std::filesystem::recursive_directory_iterator iterator{sourceRoot, error};
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end && !error) {
            const std::filesystem::directory_entry entry = *iterator;
            iterator.increment(error);
            if (!entry.is_regular_file(error))
                continue;
            const std::string extension = entry.path().extension().string();
            if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".ixx" ||
                extension == ".cppm")
                return true;
        }
        return false;
    }

    void EditorWorkspaceController::StartGameplayBuild(const bool playWhenReady) {
        LOG_INFO("editor.gameplay", "StartGameplayBuild requested (playWhenReady=%s, project='%s').", playWhenReady ? "true" : "false",
                 m_viewModel.projectRoot.c_str());
        if (m_gameplayBuilds == nullptr || m_gameplayBuildEnvironment.gameplaySdkPackage.empty()) {
            LOG_ERROR("editor.gameplay", "Gameplay build failed to start: build service or SDK package is unavailable.");
            if (playWhenReady) {
                m_viewModel.playState = EditorPlayState::Failed;
                m_viewModel.playError = "Native gameplay build service is unavailable.";
            }
            return;
        }
        const Result<Application::GameplayBuildSessionId> started = m_gameplayBuilds->Start(MakeGameplayBuildRequest());
        if (started.HasError()) {
            LOG_ERROR("editor.gameplay", "Gameplay build service failed to start build: %s", started.ErrorValue().message.c_str());
            if (playWhenReady) {
                m_viewModel.playState = EditorPlayState::Failed;
                m_viewModel.playError = started.ErrorValue().message;
            }
            return;
        }
        m_gameplayBuildSession = started.Value();
        LOG_INFO("editor.gameplay", "Gameplay build session #%" PRIu64 " started.", *m_gameplayBuildSession);
        m_playAfterGameplayBuild = m_playAfterGameplayBuild || playWhenReady;
        if (playWhenReady) {
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            m_viewModel.playState = EditorPlayState::Starting;
            m_viewModel.playError.clear();
        }
    }

    void EditorWorkspaceController::UpdateGameplayBuild(const float elapsedSeconds) {
        if (m_nativeBuildDebounceSeconds >= 0.0F && std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0F) {
            m_nativeBuildDebounceSeconds -= elapsedSeconds;
            if (m_nativeBuildDebounceSeconds <= 0.0F) {
                m_nativeBuildDebounceSeconds = -1.0F;
                StartGameplayBuild(false);
            }
        }
        if (!m_gameplayBuildSession.has_value() || m_gameplayBuilds == nullptr)
            return;
        const std::optional<Application::GameplayBuildSnapshot> snapshot = m_gameplayBuilds->Query(*m_gameplayBuildSession);
        if (!snapshot.has_value())
            return;
        switch (snapshot->state) {
            case Application::GameplayBuildState::Succeeded: {
                LOG_INFO("editor.gameplay", "Gameplay build session #%" PRIu64 " succeeded.", *m_gameplayBuildSession);
                const bool startPlay = m_playAfterGameplayBuild;
                m_gameplayBuildSession.reset();
                m_playAfterGameplayBuild = false;
                RefreshGameplayRegistry();
                if (startPlay)
                    BeginPlaySession();
                break;
            }
            case Application::GameplayBuildState::Failed:
            case Application::GameplayBuildState::Cancelled:
            case Application::GameplayBuildState::TimedOut: {
                const std::string errorMsg = snapshot->error.has_value() ? snapshot->error->message : "Gameplay build failed.";
                LOG_ERROR("editor.gameplay", "Gameplay build session #%" PRIu64 " %s: %s", *m_gameplayBuildSession,
                          snapshot->state == Application::GameplayBuildState::Failed      ? "failed"
                          : snapshot->state == Application::GameplayBuildState::Cancelled ? "was cancelled"
                                                                                          : "timed out",
                          errorMsg.c_str());
                m_gameplayBuildSession.reset();
                if (m_playAfterGameplayBuild) {
                    m_playAfterGameplayBuild = false;
                    m_viewModel.playState = EditorPlayState::Failed;
                    m_viewModel.playError = errorMsg;
                }
                break;
            }
            default:
                break;
        }
    }

    void EditorWorkspaceController::StartPlaySession() {
        LOG_INFO("editor.play_session", "StartPlaySession initiated for project '%s'.", m_viewModel.projectRoot.c_str());
        if (m_nativeGameplayReloadQuarantined) {
            m_viewModel.playState = EditorPlayState::Failed;
            m_viewModel.playError = MakeError(Gameplay::GameplayErrors::GameplayReloadRestartRequired).message;
            LOG_ERROR("editor.play_session", "%s", m_viewModel.playError.c_str());
            return;
        }
        if (HasNativeGameplaySources() && (m_gameplayBuilds == nullptr || !m_gameplayBuilds->IsUpToDate(MakeGameplayBuildRequest()))) {
            LOG_INFO("editor.play_session", "Native gameplay build required before play. Starting gameplay build...");
            StartGameplayBuild(true);
            return;
        }
        RefreshGameplayRegistry();
        BeginPlaySession();
    }

    void EditorWorkspaceController::BeginPlaySession() {
        LOG_INFO("editor.play_session", "Beginning play session for '%s'...", m_viewModel.projectRoot.c_str());
        if (m_gameplayRegistry == nullptr)
            m_gameplayRegistry = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (m_gameplayRegistry->HasBlockingDiagnostics()) {
            const std::string diagMsg = m_gameplayRegistry->Diagnostics().front().error.message;
            LOG_ERROR("editor.play_session", "Play session blocked by gameplay diagnostic: %s", diagMsg.c_str());
            m_viewModel.playState = EditorPlayState::Failed;
            m_viewModel.playError = diagMsg;
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            m_notifications.Publish("gameplay", NotificationSeverity::Error, diagMsg, "Play session blocked", "play_blocked", 0.0F,
                                    {{"Open logs", "open_logs"}});
            return;
        }

        m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
        std::unique_ptr<Runtime::RuntimeScene> preparedScene;
        if (auto cloned = m_runtimeScene.CloneActive(Runtime::SceneRuntimeId{0x8000000000000001ULL}); cloned.HasValue())
            preparedScene = std::move(cloned).Value();
        const Result<void> started = m_playSession.Start(m_document.Snapshot(), m_gameplayRegistry->Registry(), std::move(preparedScene));
        m_viewModel.activeDocumentPanelId = "horo.game";
        if (started.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode failed to start: %s", started.ErrorValue().message.c_str());
        RefreshPlayStateProjection();
        ExtractPlayViewportScene();
    }

    void EditorWorkspaceController::StopPlaySession() {
        m_playSession.Stop();
        m_nativeGameplayReloadPending = false;
        if (m_pendingGameplayRegistry && !m_pendingGameplayRegistry->HasBlockingDiagnostics()) {
            m_gameplayRegistry = std::move(m_pendingGameplayRegistry);
            RefreshAvailableBehaviorProjection();
        } else {
            m_pendingGameplayRegistry.reset();
        }
        m_viewModel.playError.clear();
        m_viewModel.playState = EditorPlayState::Idle;
        m_viewModel.activeDocumentPanelId = m_prePlayDocumentPanelId.empty() ? "horo.viewport" : m_prePlayDocumentPanelId;
        m_activeRuntimeRevision = {};
    }

    void EditorWorkspaceController::RefreshPlayStateProjection() {
        switch (m_playSession.State()) {
            case EditorPlaySessionState::Idle:
                m_viewModel.playState = EditorPlayState::Idle;
                break;
            case EditorPlaySessionState::Starting:
                m_viewModel.playState = EditorPlayState::Starting;
                break;
            case EditorPlaySessionState::Playing:
                m_viewModel.playState = EditorPlayState::Playing;
                break;
            case EditorPlaySessionState::Paused:
                m_viewModel.playState = EditorPlayState::Paused;
                break;
            case EditorPlaySessionState::Reloading:
                m_viewModel.playState = EditorPlayState::Starting;
                break;
            case EditorPlaySessionState::Stopping:
                m_viewModel.playState = EditorPlayState::Stopping;
                break;
            case EditorPlaySessionState::Failed:
                m_viewModel.playState = EditorPlayState::Failed;
                break;
        }
        m_viewModel.playError = m_playSession.LastError().has_value() ? m_playSession.LastError()->message : std::string{};
    }

    void EditorWorkspaceController::UpdatePlayPresentation(const float elapsedSeconds) {
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0F)
            return;
        m_playSession.PresentationUpdate(Gameplay::FrameDeltaTime{static_cast<double>(elapsedSeconds)});
        ExtractPlayViewportScene();
    }

    void EditorWorkspaceController::UpdatePlayFixed(const std::span<const Gameplay::GameplayInputAction> input,
                                                    const double fixedDeltaSeconds) {
        if (!std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0)
            return;
        ApplyNativeGameplayReload();
        ApplyPendingGameplayRegistry();
        if (const Result<void> updated = m_playSession.FixedUpdate(input, Gameplay::FixedDeltaTime{fixedDeltaSeconds}); updated.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode fixed update failed: %s", updated.ErrorValue().message.c_str());
        RefreshPlayStateProjection();
        ExtractPlayViewportScene();
    }

    EditorViewportCamera EditorWorkspaceController::ResolvePlayViewportCamera(const Runtime::RuntimeSceneView &runtimeView) const {
        EditorViewportCamera camera = m_viewport.Current().camera;
        for (std::size_t slot = 0; slot < runtimeView.SlotCount(); ++slot) {
            const std::optional<Runtime::RuntimeEntityView> entity = runtimeView.EntityAt(slot);
            if (!entity || !entity->components->camera.has_value())
                continue;
            const Runtime::CameraComponent &authoredCamera = *entity->components->camera;
            const Math::Transform worldTransform = ResolveRuntimeEntityTransform(runtimeView, *entity);
            return EditorViewportCamera{
                .projection = authoredCamera.projection,
                .position = worldTransform.translation,
                .target = worldTransform.translation + worldTransform.rotation.Rotate({0.0F, 0.0F, -1.0F}),
                .up = worldTransform.rotation.Rotate({0.0F, 1.0F, 0.0F}),
                .verticalFovRadians = authoredCamera.verticalFieldOfViewRadians,
                .orthographicHeight = authoredCamera.orthographicHeight,
                .nearPlane = authoredCamera.nearPlane,
                .farPlane = authoredCamera.farPlane,
            };
        }
        return camera;
    }

    void EditorWorkspaceController::ExtractPlayViewportScene() {
        const Runtime::RuntimeScene *scene = m_playSession.Scene();
        if (scene == nullptr)
            return;
        const Runtime::RuntimeSceneView runtimeView = scene->View();
        const EditorViewportCamera camera = ResolvePlayViewportCamera(runtimeView);
        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        Result<EditorViewportSceneSnapshot> extracted =
            ExtractEditorViewportScene(runtimeView, m_playSession.AuthoringRevision(), camera, m_primitiveMeshCache, &document,
                                       &m_assetMeshCache, false);
        if (extracted.HasError()) {
            LOG_ERROR("editor.play_mode", "Game viewport extraction failed: %s", extracted.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene = std::move(extracted).Value();
        ++m_viewportSceneRevision;
        m_viewModel.viewportCamera = camera;
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::RefreshSelectionProjection() {
        const SelectionSnapshot &selection = m_selection.Current();
        m_viewModel.primarySelection = selection.primary;
        m_viewModel.selectedObjects = selection.objects;
        m_viewModel.viewportCamera = m_viewportScene.camera;
        m_viewModel.primarySelectionWorldTransform.reset();
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        m_viewModel.primarySelectionParentWorldTransform.reset();
        m_viewModel.primarySelectionWorldBounds.reset();
        if (const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
            selection.primary && active && m_viewportScene.runtimeSceneId == active->RuntimeId()) {
            const Result<SceneObjectWorldTransforms> transforms = ResolveSceneObjectWorldTransforms(*active, *selection.primary);
            if (transforms.HasValue()) {
                m_viewModel.primarySelectionWorldTransform = transforms.Value().localToWorld;
                m_viewModel.primarySelectionParentWorldTransform = transforms.Value().parentToWorld;
            } else {
                LOG_ERROR("editor.viewport", "Selected object transform projection failed: %s", transforms.ErrorValue().message.c_str());
            }
        }
        if (m_viewportScene.instances.size() != m_viewportScene.instanceObjects.size()) {
            return;
        }
        for (std::size_t index = 0; index < m_viewportScene.instances.size(); ++index) {
            m_viewportScene.instances[index].presentation.tint = {0.12F, 0.72F, 1.0F};
            m_viewportScene.instances[index].presentation.tintStrength =
                std::ranges::find(selection.objects, m_viewportScene.instanceObjects[index]) != selection.objects.end() ? 0.65F : 0.0F;
            if (selection.primary == m_viewportScene.instanceObjects[index]) {
                const Result<Math::Aabb> bounds =
                    Math::TransformAabb(m_viewportScene.instances[index].localBounds, m_viewportScene.instances[index].localToWorld);
                if (bounds.HasValue())
                    m_viewModel.primarySelectionWorldBounds = bounds.Value();
                else
                    LOG_ERROR("editor.viewport", "Selected object bounds projection failed: %s", bounds.ErrorValue().message.c_str());
            }
        }
    }

    void EditorWorkspaceController::RefreshViewportLightProjection() {
        m_viewModel.viewportLights.clear();
        if (m_viewportScene.lights.size() != m_viewportScene.lightObjects.size())
            return;
        m_viewModel.viewportLights.reserve(m_viewportScene.lights.size());
        for (std::size_t index = 0; index < m_viewportScene.lights.size(); ++index) {
            m_viewModel.viewportLights.emplace_back(m_viewportScene.lightObjects[index], m_viewportScene.lights[index]);
        }
    }
}  // namespace Horo::Editor
