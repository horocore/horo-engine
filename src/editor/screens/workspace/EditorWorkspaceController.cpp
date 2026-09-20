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
        : m_runtimeScene(runtimeScene), m_assetRegistry(assetRegistry), m_sourceOpenService(projectRoot),
          m_mutableAssetRegistry(dependencies.mutableAssetRegistry), m_mutations(dependencies.mutations),
          m_durableFiles(dependencies.durableFiles), m_importerCatalog(dependencies.importerCatalog),
          m_assetPreviews(dependencies.jobs != nullptr ? std::make_unique<Assets::AssetPreviewService>(*dependencies.jobs) : nullptr),
          m_sourceOpenNavigator(dependencies.sourceOpenNavigator), m_diagnosticSourceNavigator(dependencies.diagnosticSourceNavigator),
          m_gameplayBuilds(dependencies.gameplayBuilds), m_gameplayBuildEnvironment(dependencies.gameplayBuildEnvironment),
          m_localization(dependencies.localization),
          m_sceneFileWatch(dependencies.jobs != nullptr ? std::make_unique<SceneFileWatchService>(*dependencies.jobs) : nullptr) {
        if (!m_diagnosticSourceNavigator) {
            m_diagnosticSourceNavigator = [](const DiagnosticSourceRequest &source) {
                const std::filesystem::path path{source.absolutePath};
                return OpenInExternalEditor(path) || RevealInNativeFileManager(path);
            };
        }
        const std::filesystem::path absoluteProjectRoot = ResolveProjectRoot(projectRoot);
        InitializeProjectState(absoluteProjectRoot);
        InitializeWorkspaceLayout();
        InitializeInitialScene(absoluteProjectRoot);
        RefreshSceneProjections();
    }

    void EditorWorkspaceController::InitializeProjectState(const std::filesystem::path &absoluteProjectRoot) {
        m_viewModel.projectRoot = absoluteProjectRoot.string();
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(m_viewModel.projectRoot, {});
        if (m_durableFiles != nullptr) {
            ProjectIntegrityValidatorService validator{*m_durableFiles};
            if (const Result<void> repaired = validator.Repair(absoluteProjectRoot); repaired.HasError())
                LOG_ERROR("editor.project_validator", "Project integrity repair failed for '%s': %s", absoluteProjectRoot.string().c_str(),
                          repaired.ErrorValue().message.c_str());
        }
        m_gameplayRegistry = ProjectGameplayRegistry::Discover(absoluteProjectRoot);
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->Diagnostics())
            LOG_ERROR("editor.gameplay", "Gameplay source '%s' is invalid: %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
        for (const Gameplay::BehaviorRegistration &registration : m_gameplayRegistry->Registry().Registrations())
            m_viewModel.availableBehaviors.push_back(registration.descriptor);
    }

    void EditorWorkspaceController::InitializeWorkspaceLayout() {
        m_viewModel.panelDockAreas = {{"horo.hierarchy", WorkspaceDockArea::Left},  {"horo.viewport", WorkspaceDockArea::Document},
                                      {"horo.game", WorkspaceDockArea::Document},   {"horo.global_dock", WorkspaceDockArea::Bottom},
                                      {"horo.inspector", WorkspaceDockArea::Right}, {"horo.input_mapping", WorkspaceDockArea::Right}};
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.hierarchy", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.viewport", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.game", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 1}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.global_dock", ActivityBarSlot{ActivityBarRail::Left, 2, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.inspector", ActivityBarSlot{ActivityBarRail::Right, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.input_mapping", ActivityBarSlot{ActivityBarRail::Right, 1, 0}));
    }

    void EditorWorkspaceController::InitializeInitialScene(const std::filesystem::path &absoluteProjectRoot) {
        if (!LoadInitialScene(absoluteProjectRoot) && !m_initializationError.has_value())
            CreateBootstrapScene();
        if (m_defaultScenePath.has_value())
            InspectInitialSceneRecovery(absoluteProjectRoot);
    }

    bool EditorWorkspaceController::LoadInitialScene(const std::filesystem::path &absoluteProjectRoot) {
        const Result<std::optional<LoadedProjectScene>> loaded = LoadProjectDefaultScene(absoluteProjectRoot);
        if (loaded.HasError()) {
            m_initializationError = loaded.ErrorValue();
            LOG_ERROR("editor.scene_document", "Default scene load failed: %s", loaded.ErrorValue().message.c_str());
            return false;
        }
        if (!loaded.Value().has_value())
            return false;
        LoadedProjectScene projectScene = *loaded.Value();
        if (const Result<void> installed = m_document.LoadSaved(std::move(projectScene.objects), std::move(projectScene.prefabInstances));
            installed.HasError()) {
            m_initializationError = installed.ErrorValue();
            LOG_ERROR("editor.scene_document", "Default scene validation failed: %s", installed.ErrorValue().message.c_str());
            return false;
        }
        m_defaultScenePath = std::move(projectScene.absolutePath);
        m_sceneFingerprint = std::move(projectScene.fingerprint);
        m_history.Clear();
        LOG_INFO("editor.scene_document", "Loaded default scene '%s'.", m_defaultScenePath->string().c_str());
        return true;
    }

    void EditorWorkspaceController::CreateBootstrapScene() {
        const Math::Quaternion pitch = Math::Quaternion::FromAxisAngle({1.0F, 0.0F, 0.0F}, -0.42F);
        const Math::Quaternion yaw = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 0.55F);
        const Result<SceneCommandResult> created = m_documentCommands.Execute(CreateSceneObjectCommand{
            .name = "Box",
            .localTransform = Math::Transform{.rotation = pitch * yaw},
            .primitiveMesh = PrimitiveMeshDescriptor{},
        });
        if (created.HasError()) {
            LOG_ERROR("editor.scene_document", "Bootstrap scene creation failed: %s", created.ErrorValue().message.c_str());
            return;
        }
        static_cast<void>(m_document.MarkSaved(m_document.Revision(), m_document.State()));
        m_history.Clear();
    }

    void EditorWorkspaceController::InspectInitialSceneRecovery(const std::filesystem::path &absoluteProjectRoot) {
        const Result<std::optional<ProjectSceneRecoveryRecord>> recovery =
            InspectProjectSceneRecovery(absoluteProjectRoot, *m_defaultScenePath);
        if (recovery.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery inspection failed: %s", recovery.ErrorValue().message.c_str());
            return;
        }
        if (recovery.Value().has_value()) {
            m_viewModel.recoveryAvailable = true;
            LOG_WARN("editor.scene_recovery", "Validated recovery is available for '%s'; canonical scene was not modified.",
                     m_defaultScenePath->string().c_str());
        }
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

    bool EditorWorkspaceController::CommitSceneSave(const SceneDocumentSnapshot &snapshot, const SceneFileFingerprint &fingerprint,
                                                    const std::optional<std::filesystem::path> &newActivePath) {
        if (const Result<void> marked = m_document.MarkSaved(snapshot.revision, snapshot.state); marked.HasError()) {
            LOG_ERROR("editor.scene_document", "Saved scene state could not be acknowledged: %s", marked.ErrorValue().message.c_str());
            return false;
        }
        if (newActivePath.has_value())
            m_defaultScenePath = *newActivePath;
        m_sceneFingerprint = fingerprint;
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
        return true;
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

        if (!CommitSceneSave(snapshot, saved.Value().fingerprint))
            return;
        LOG_INFO("editor.scene_document", "Saved scene revision %llu to '%s'.", static_cast<unsigned long long>(snapshot.revision.value),
                 m_defaultScenePath->string().c_str());
    }

    std::optional<std::filesystem::path> EditorWorkspaceController::ResolveSceneSaveDestination(
        const std::filesystem::path &absolutePath) const {
        std::error_code canonicalError;
        const std::filesystem::path destination = std::filesystem::weakly_canonical(absolutePath, canonicalError);
        if (canonicalError) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' could not be normalized: %s",
                      absolutePath.string().c_str(), canonicalError.message().c_str());
            return std::nullopt;
        }
        if (!destination.is_absolute()) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because '%s' is not absolute.", destination.string().c_str());
            return std::nullopt;
        }
        return destination;
    }

    /** @copydoc EditorWorkspaceController::SaveSceneToPath */
    void EditorWorkspaceController::SaveSceneToPath(const std::filesystem::path &absolutePath, const bool copyOnly) {
        if (!m_defaultScenePath.has_value() || m_mutations == nullptr || m_durableFiles == nullptr) {
            LOG_ERROR("editor.scene_document", "Destination save rejected because the active scene or "
                                               "durable writer services are unavailable.");
            return;
        }

        const std::optional<std::filesystem::path> destination = ResolveSceneSaveDestination(absolutePath);
        if (!destination.has_value())
            return;
        const std::filesystem::path activePath = m_defaultScenePath->lexically_normal();
        if (copyOnly && *destination == activePath) {
            LOG_ERROR("editor.scene_document", "Save Copy As rejected because the destination is the active scene path '%s'.",
                      activePath.string().c_str());
            return;
        }
        if (!copyOnly && *destination == activePath) {
            SaveScene(true);
            return;
        }

        const SceneDocumentSnapshot snapshot = m_document.Snapshot();
        auto saved = SaveProjectSceneToPath(std::filesystem::path{m_viewModel.projectRoot}, *destination, snapshot, true, *m_mutations,
                                            *m_durableFiles);
        if (saved.HasError()) {
            LOG_ERROR("editor.scene_document", "%s failed for '%s': %s", copyOnly ? "Save Copy As" : "Save As",
                      destination->string().c_str(), saved.ErrorValue().message.c_str());
            return;
        }
        if (saved.Value().status != ProjectSceneDestinationSaveStatus::Saved) {
            LOG_WARN("editor.scene_document", "%s paused because destination '%s' changed during the save.",
                     copyOnly ? "Save Copy As" : "Save As", destination->string().c_str());
            return;
        }

        if (copyOnly) {
            LOG_INFO("editor.scene_document", "Saved scene copy revision %llu to '%s'.",
                     static_cast<unsigned long long>(snapshot.revision.value), destination->string().c_str());
            return;
        }

        if (!CommitSceneSave(snapshot, saved.Value().fingerprint, *destination))
            return;
        LOG_INFO("editor.scene_document", "Saved scene revision %llu as '%s'; active document identity was updated.",
                 static_cast<unsigned long long>(snapshot.revision.value), destination->string().c_str());
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

    std::vector<SceneObjectId> EditorWorkspaceController::CollectExistingDeleteObjects(const std::vector<SceneObjectId> &objects,
                                                                                       std::string &singleName,
                                                                                       std::size_t &requestedCount) const {
        std::vector<SceneObjectId> requested;
        requested.reserve(objects.size());
        for (const SceneObjectId object : objects) {
            if (object.IsValid() && std::ranges::find(requested, object) == requested.end())
                requested.push_back(object);
        }
        requestedCount = requested.size();

        std::vector<SceneObjectId> existing;
        existing.reserve(requested.size());
        const std::span<const SceneObjectSnapshot> documentObjects = m_document.Objects();
        for (const SceneObjectId object : requested) {
            const auto found = std::ranges::find(documentObjects, object, &SceneObjectSnapshot::id);
            if (found == documentObjects.end())
                continue;
            existing.push_back(object);
            if (existing.size() == 1)
                singleName = found->name;
        }
        return existing;
    }

    void EditorWorkspaceController::HandleDeleteSelectedObjects(const std::vector<SceneObjectId> &objects) {
        std::string singleName;
        std::size_t requestedCount = 0;
        std::vector<SceneObjectId> existing = CollectExistingDeleteObjects(objects, singleName, requestedCount);
        const std::size_t skipped = requestedCount - existing.size();
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

}  // namespace Horo::Editor
