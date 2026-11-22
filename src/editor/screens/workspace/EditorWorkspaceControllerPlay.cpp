#include "EditorPlayXrHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/NotificationService.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "WorkspacePathText.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <array>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
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
    }  // namespace

    Application::GameplayBuildRequest EditorWorkspaceController::MakeGameplayBuildRequest() const {
        return Application::GameplayBuildRequest{
            .projectRoot = ProjectRoot(),
            .environment = m_gameplayBuildEnvironment,
        };
    }

    bool EditorWorkspaceController::HasNativeGameplaySources() const {
        const std::filesystem::path sourceRoot = ProjectRoot() / "source" / "gameplay";
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
            const std::string extension = WorkspacePathText(entry.path().extension());
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
            InvalidateAgentViewport();
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            SyncPanelHost(WorkspaceDockArea::Document, "horo.game");
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
                PrepareSourceLanguage();
                if (startPlay)
                    BeginPlaySession();
                break;
            }
            case Application::GameplayBuildState::Failed:
            case Application::GameplayBuildState::Cancelled:
            case Application::GameplayBuildState::TimedOut: {
                const std::string errorMsg = snapshot->error.has_value() ? snapshot->error->message : "Gameplay build failed.";
                const char *outcome = "timed out";
                if (snapshot->state == Application::GameplayBuildState::Failed)
                    outcome = "failed";
                else if (snapshot->state == Application::GameplayBuildState::Cancelled)
                    outcome = "was cancelled";
                LOG_ERROR("editor.gameplay", "Gameplay build session #%" PRIu64 " %s: %s", *m_gameplayBuildSession, outcome,
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
        InvalidateAgentViewport();
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
        StopCameraCutPreview();
        LOG_INFO("editor.play_session", "Beginning play session for '%s'...", m_viewModel.projectRoot.c_str());
        if (m_gameplayRegistry == nullptr)
            m_gameplayRegistry = ProjectGameplayRegistry::Discover(ProjectRoot(), m_gameplayRetirements);
        if (m_gameplayRegistry->HasBlockingDiagnostics()) {
            const std::string diagMsg = m_gameplayRegistry->Diagnostics().front().error.message;
            LOG_ERROR("editor.play_session", "Play session blocked by gameplay diagnostic: %s", diagMsg.c_str());
            m_viewModel.playState = EditorPlayState::Failed;
            m_viewModel.playError = diagMsg;
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            SyncPanelHost(WorkspaceDockArea::Document, "horo.game");
            m_notifications.Publish("gameplay", NotificationSeverity::Error, diagMsg, "Play session blocked", "play_blocked", 0.0F,
                                    {{"Open logs", "open_logs"}});
            return;
        }

        m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
        LoadDocumentAssetMeshes();  // Cold SAME cache preparation before the XR Scene participant captures exact leases.
        BindPlaySaveSources();
        const auto desktopView = m_mountedWorldPlaySelected ? EditorPlayViewSource::MountedWorld : EditorPlayViewSource::AuthoredCamera;
        const Result<void> started =
            m_playSession.Start(m_document.Snapshot(), m_gameplayRegistry->Registry(), m_gameplayRegistry->Components(), m_playSceneFactory,
                                m_triggerBinding, m_selectedXrPlayHost ? m_selectedXrPlayHost->ViewSource() : desktopView,
                                m_selectedXrPlayHost,
                                m_defaultScenePath ? m_defaultScenePath->lexically_relative(ProjectRoot()) : std::filesystem::path{},
                                &m_primitiveMeshCache, {.decision = [this] {
            return m_gameplayRegistry->AcquireDecisionAuthoringGeneration();
        }, .perception = [this] {
            return m_gameplayRegistry->AcquirePlayPerceptionGeneration();
        }});
        m_viewModel.activeDocumentPanelId = "horo.game";
        SyncPanelHost(WorkspaceDockArea::Document, "horo.game");
        if (started.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode failed to start: %s", started.ErrorValue().message.c_str());
        RefreshPlayStateProjection();
        ExtractPlayViewportScene();
    }

    /** @copydoc EditorWorkspaceController::BindPlaySaveSources */
    void EditorWorkspaceController::BindPlaySaveSources() {
        auto bindings = m_gameplayRegistry->AcquireSaveBindings();
        if (bindings.HasError()) {
            m_playSession.ConfigureSaveError(bindings.ErrorValue());
            return;
        }
        m_playSession.ConfigureSaveBindings(std::move(bindings).Value());
    }

    void EditorWorkspaceController::StopPlaySession() {
        m_playSession.Stop();
        RefreshCollisionActivation();
        m_nativeGameplayReloadPending = false;
        FinishStoppedGameplayRegistry();
        RefreshPlayStateProjection();
        m_viewModel.activeDocumentPanelId = m_prePlayDocumentPanelId.empty() ? "horo.viewport" : m_prePlayDocumentPanelId;
        SyncPanelHost(WorkspaceDockArea::Document, m_viewModel.activeDocumentPanelId);
        m_activeRuntimeRevision = {};
    }

    /** @copydoc EditorWorkspaceController::FinishStoppedGameplayRegistry */
    void EditorWorkspaceController::FinishStoppedGameplayRegistry() {
        if (m_playSession.State() != EditorPlaySessionState::Idle)
            return;
        if (m_pendingGameplayRegistry && !m_pendingGameplayRegistry->HasBlockingDiagnostics()) {
            m_gameplayRegistry = std::move(m_pendingGameplayRegistry);
            RefreshAvailableBehaviorProjection();
        } else {
            m_pendingGameplayRegistry.reset();
        }
    }

    void EditorWorkspaceController::RefreshPlayStateProjection() {
        RefreshCollisionActivation();
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
        if (m_viewModel.playState != EditorPlayState::Idle)
            InvalidateAgentViewport();
        m_viewModel.playError = m_playSession.LastError().has_value() ? m_playSession.LastError()->message : std::string{};
        RefreshPlayDecisionProblem();
        const auto scene = m_playSession.SceneView();
        const auto selected = m_selection.Current().primary;
        const auto owner = scene && selected ? scene->Find(Runtime::SceneObjectId{selected->value}) : std::nullopt;
        m_playSession.RefreshAgentInspection(owner);
        RefreshLiveEnvironmentQueryPreview(owner);
        const auto history = m_playSession.AgentInspectionHistory();
        m_viewModel.playAgentInspection.assign(history.begin(), history.end());
        m_viewModel.playDecisionDebug = m_playSession.ReadDecisionDebugState();
    }

    /** @copydoc EditorWorkspaceController::SetPlayDecisionInspectorVisibility */
    void EditorWorkspaceController::SetPlayDecisionInspectorVisibility(const std::optional<AI::AgentHandle> agent) {
        m_playSession.SetDecisionDebugView(agent);
        m_viewModel.playDecisionDebug = m_playSession.ReadDecisionDebugState();
    }

    /** @copydoc EditorWorkspaceController::ResolvePlayDecisionProblemObject */
    std::optional<SceneObjectId> EditorWorkspaceController::ResolvePlayDecisionProblemObject(
        const EditorPlayDecisionProblem &problem) const {
        if (!problem.entity || !problem.entity->IsValid() || problem.entity->runtime != problem.scene ||
            m_viewModel.playViewSource == EditorPlayViewSource::MountedWorld || m_playSession.AuthoringRevision() != m_document.Revision())
            return std::nullopt;
        const auto scene = m_playSession.SceneView();
        if (!scene || !scene->IsCurrent() || scene->RuntimeId() != problem.scene)
            return std::nullopt;
        const auto entity = scene->Get(*problem.entity);
        if (entity.HasError() || !entity.Value().authoredObject || !entity.Value().components || !entity.Value().components->aiAgent ||
            entity.Value().components->aiAgent->agent != problem.agent)
            return std::nullopt;
        const SceneObjectId object{entity.Value().authoredObject->value};
        if (!m_document.Contains(object))
            return std::nullopt;
        return object;
    }

    /** @copydoc EditorWorkspaceController::RefreshPlayDecisionProblem */
    void EditorWorkspaceController::RefreshPlayDecisionProblem() {
        auto &problem = m_viewModel.playDecisionProblem;
        const auto scene = m_playSession.SceneView();
        const bool playing = m_viewModel.playState == EditorPlayState::Playing || m_viewModel.playState == EditorPlayState::Paused;
        if (!playing || !scene || !scene->IsCurrent()) {
            problem.reset();
            m_playDecisionDiagnosticSequence = 0;
            m_viewModel.playDecisionGraphFocus.reset();
            m_viewModel.playDecisionGraphUnavailable = false;
            m_viewModel.playDecisionObjectAvailable = false;
            return;
        }
        if (problem && problem->scene != scene->RuntimeId()) {
            problem.reset();
            m_playDecisionDiagnosticSequence = 0;
        }
        if (auto captured = m_playSession.ReadDecisionProblem(); captured && captured->scene == scene->RuntimeId() && captured->tick > 0 &&
                                                                 captured->agent.IsValid() &&
                                                                 (!problem || captured->tick > problem->tick)) {
            problem = std::move(captured);
            m_viewModel.playDecisionGraphFocus.reset();
            m_viewModel.playDecisionGraphUnavailable = false;
            if (m_diagnosticOutput) {
                const auto &error = problem->error;
                m_playDecisionDiagnosticSequence = m_diagnosticOutput->AppendWithSequence(
                    {.severity = DiagnosticSeverityForError(error.severity).value_or(DiagnosticSeverity::Note),
                     .stage = std::string{PlayDecisionDiagnosticStage},
                     .code = DiagnosticCode{error.code.Value()},
                     .message = error.message});
            }
        }
        m_viewModel.playDecisionObjectAvailable = problem && ResolvePlayDecisionProblemObject(*problem).has_value();
    }

    /** @copydoc EditorWorkspaceController::NavigatePlayDecisionDiagnostic */
    void EditorWorkspaceController::NavigatePlayDecisionDiagnostic(const std::uint64_t sequence) {
        const auto unavailable = [this] {
            constexpr auto message = "workspace.global_dock.build_output.source.invalid";
            m_viewModel.contentBrowserOperationError = message;
            if (m_localization)
                NotificationService{DataBus()}.Publish(PlayDecisionDiagnosticStage, NotificationSeverity::Warning,
                                                       m_localization->Get("editor", message), {}, message);
        };
        const auto &problem = m_viewModel.playDecisionProblem;
        if (!problem || sequence == 0 || sequence != m_playDecisionDiagnosticSequence || !problem->graph.IsValid() ||
            !problem->node.IsValid()) {
            unavailable();
            return;
        }
        const auto previousFocus = m_playDecisionGraphFocusSequence;
        NavigatePlayDecisionGraphProblem({problem->scene, problem->tick, problem->agent, problem->graph, problem->node});
        if (m_playDecisionGraphFocusSequence == previousFocus)
            unavailable();
    }

    /** @copydoc EditorWorkspaceController::NavigatePlayDecisionProblem */
    void EditorWorkspaceController::NavigatePlayDecisionProblem(const PlayDecisionNavigationRequest &request) {
        const auto &problem = m_viewModel.playDecisionProblem;
        const auto state = m_playSession.State();
        if ((state != EditorPlaySessionState::Playing && state != EditorPlaySessionState::Paused) || !problem || !problem->entity ||
            request.tick != problem->tick || request.agent != problem->agent || request.entity != *problem->entity)
            return;
        const auto object = ResolvePlayDecisionProblemObject(*problem);
        if (!object) {
            m_viewModel.playDecisionObjectAvailable = false;
            return;
        }
        if (m_selection.SetObjects({*object}, object).HasError()) {
            m_viewModel.playDecisionObjectAvailable = false;
            return;
        }
        m_viewModel.hierarchyRevealObject = object;
        m_viewModel.hierarchyRevealRevision = m_document.Revision();
        RefreshSelectionProjection();
    }

    void EditorWorkspaceController::UpdatePlayPresentation(const float elapsedSeconds) {
        RefreshWorldSourceProjection();
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0F)
            return;
        m_playSession.PresentationUpdate(Gameplay::FrameDeltaTime{static_cast<double>(elapsedSeconds)});
        FinishStoppedGameplayRegistry();
        ResumePlaySceneReplacement();
        RefreshPlayStateProjection();
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

    Result<void> EditorWorkspaceController::BindPlayRuntimeDispatch(const Runtime::RuntimeDispatchSource &source) {
        return m_playSession.BindRuntimeDispatch(source);
    }

    void EditorWorkspaceController::UpdatePlayFixed(const std::span<const Gameplay::GameplayInputAction> input,
                                                    const Runtime::FixedStepContext &context) {
        ApplyNativeGameplayReload();
        ApplyPendingGameplayRegistry();
        if (auto updated = m_playSession.OnRuntimeFixedUpdate(input, context); updated.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode fixed update failed: %s", updated.ErrorValue().message.c_str());
        // Presentation projection/extraction waits for ordinary update, after the real commit or abort.
    }

    void EditorWorkspaceController::PreparePlayFixedCommit(const Runtime::FixedStepContext &context) {
        if (auto prepared = m_playSession.PrepareRuntimeFixedCommit(context); prepared.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode fixed preparation failed: %s", prepared.ErrorValue().message.c_str());
    }

    void EditorWorkspaceController::CommitPlayFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        m_playSession.CommitRuntimeFixedUpdate(context);
    }

    void EditorWorkspaceController::AbortPlayFixedUpdate(const Runtime::FixedStepContext &context) noexcept {
        m_playSession.AbortRuntimeFixedUpdate(context);
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
        if (m_playSession.UsesMountedViewport()) {
            if (m_viewportScene.runtimeSceneId.IsValid())
                ClearSimulatorViewport();
            return;
        }
        const auto playScene = m_playSession.SceneView();
        if (!playScene)
            return;
        if (!m_selectedXrPlayHost) {
            ExtractPlayViewportWithCamera(*playScene, ResolvePlayViewportCamera(*playScene));
            return;
        }
        if (const auto updated = m_selectedXrPlayHost->UpdateScene(*playScene); updated.HasError()) {
            ClearSimulatorViewport();
            LOG_ERROR("editor.xr", "XR Scene presentation revoked: %s", updated.ErrorValue().message.c_str());
            StopPlaySession();
            return;
        }
        if (m_selectedXrPlayHost->ViewSource() != EditorPlayViewSource::Simulator)
            return;
        const auto preview = m_selectedXrPlayHost->Preview();
        if (!preview) {
            ClearSimulatorViewport();
            return;
        }
        const EditorViewportCamera camera{.position = preview->position,
                                          .target = preview->position + preview->orientation.Rotate({0, 0, -1}),
                                          .up = preview->orientation.Rotate({0, 1, 0}),
                                          .verticalFovRadians = preview->verticalFovRadians,
                                          .nearPlane = preview->nearPlane,
                                          .farPlane = preview->farPlane};
        ExtractPlayViewportWithCamera(*playScene, camera);
    }

    void EditorWorkspaceController::ClearSimulatorViewport() {
        m_viewModel.simulatorPreviewAvailable = false;
        m_viewportScene = {};
        ++m_viewportSceneRevision;
    }

    void EditorWorkspaceController::ExtractPlayViewportWithCamera(const Runtime::RuntimeSceneView &runtimeView,
                                                                  const EditorViewportCamera &camera) {
        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        auto extracted = ExtractEditorViewportScene(runtimeView, m_playSession.AuthoringRevision(), camera, m_primitiveMeshCache, &document,
                                                    &m_assetMeshCache, false);
        if (extracted.HasError()) {
            if (m_selectedXrPlayHost)
                ClearSimulatorViewport();
            LOG_ERROR("editor.play_mode", "Game viewport extraction failed: %s", extracted.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene = std::move(extracted).Value();
        ++m_viewportSceneRevision;
        m_viewModel.viewportCamera = camera;
        m_viewModel.simulatorPreviewAvailable =
            m_selectedXrPlayHost && m_selectedXrPlayHost->ViewSource() == EditorPlayViewSource::Simulator;
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::RefreshSelectionProjection() {
        ReconcileAudioPreview();
        if (m_viewModel.physicsSelectionRevision != m_selection.Current().revision.value)
            m_viewModel.physicsEditError.clear();
        m_viewModel.physicsSelectionRevision = m_selection.Current().revision.value;
        const SelectionSnapshot &selection = m_selection.Current();
        if (m_viewModel.selectionRevision != selection.revision.value)
            InvalidateAgentSceneSelection();
        if (selection.primary || !selection.objects.empty())
            m_viewModel.prefabSelection.reset();
        m_viewModel.primarySelection = selection.primary;
        m_viewModel.selectedObjects = selection.objects;
        m_viewModel.selectionRevision = selection.revision.value;
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
