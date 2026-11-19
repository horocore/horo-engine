#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Camera/CameraErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <limits>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief The baseline preview surface admits camera tracks without inventing other owner bindings. */
        bool EligiblePreview(const Cinematic::SequencePlaybackActivation &activation, const bool playActive) noexcept {
            return !playActive && activation.plan.TrackCount() == 0 && activation.plan.EventCount() == 0;
        }

        /** @brief Distinguishes an authored transition refusal from ordinary preview binding loss. */
        bool IsCameraTransitionRefusal(const Error &error) noexcept {
            using namespace Runtime::CameraErrors;
            return error.domain.Value() == UnsupportedTransition.domain.Value() &&
                   (error.code.Value() == UnsupportedTransition.code.Value() ||
                    error.code.Value() == TransitionBudgetExceeded.code.Value() ||
                    error.code.Value() == IncompatibleTransition.code.Value());
        }

        /** @brief Copies the current editor owner's base camera; never captures a pre-cinematic value. */
        Runtime::CameraProposal AuthoringProposal(const EditorViewportCamera &camera, const std::uint64_t sceneRevision) {
            const auto kind = camera.projection == Runtime::CameraProjection::Perspective ? Render::RenderProjectionKind::Perspective
                                                                                          : Render::RenderProjectionKind::Orthographic;
            Runtime::CameraProposal proposal{{},
                                             {camera.position,
                                              camera.target,
                                              camera.up,
                                              {kind, camera.verticalFovRadians, camera.orthographicHeight, camera.nearPlane,
                                               camera.farPlane}}};
            proposal.sceneRevision = sceneRevision;
            return proposal;
        }

        /** @brief Projects the committed copied runtime values into the existing viewport extraction contract. */
        EditorViewportCamera ViewportCamera(const Render::RenderCameraView &camera) {
            const auto kind = camera.projection.kind == Render::RenderProjectionKind::Perspective ? Runtime::CameraProjection::Perspective
                                                                                                  : Runtime::CameraProjection::Orthographic;
            return {kind,
                    camera.position,
                    camera.target,
                    camera.up,
                    camera.projection.verticalFovRadians,
                    camera.projection.orthographicHeight,
                    camera.projection.nearPlane,
                    camera.projection.farPlane};
        }
    }  // namespace

    EditorWorkspaceController::CameraPreviewState::CameraPreviewState(Cinematic::CinematicRuntimeService &&runtimeValue,
                                                                      Runtime::CameraService &&cameraValue)
        : runtime(std::move(runtimeValue)), camera(std::move(cameraValue)),
          crossings(runtime.ServiceSnapshot().budget.maximumBoundaryOccurrences) {}

    EditorWorkspaceController::~EditorWorkspaceController() {
        StopBlackboardUsageReview();
        StopEnvironmentQueryPreview();
        static_cast<void>(DrainUiCanvasRecovery());
        static_cast<void>(DrainPrefabRecovery());
        StopPrefabExternalWatch();
        static_cast<void>(ShutdownTerrainDocuments(true));
        ShutdownTerrainCook();
        static_cast<void>(ShutdownPrefabSettings(true));
        DetachWorldSourceOwner();
        if (m_worldBake)
            m_worldBake->Close();
        // Preview providers may borrow host project authority. Join them before any document/service retirement.
        if (m_assetPreviews)
            m_assetPreviews->Shutdown();
        m_pendingContentBrowserPreviews.clear();
        StopAudioPreview();
        m_prefabCancellation.RequestCancellation();
        RetirePrefabHierarchy();
        static_cast<void>(ShutdownCollisionSettings(true));
        (void)ShutdownGameplayModules();
    }

    /** @copydoc EditorWorkspaceController::ShutdownGameplayModules */
    Result<void> EditorWorkspaceController::ShutdownGameplayModules() {
        m_playSceneReplacement.reset();
        m_playSession.RetireOnLeave(m_gameplayRegistry);
        m_pendingGameplayRegistry.reset();
        m_gameplayRegistry.reset();
        if (m_playSaveRetirement && m_playSaveRetirement->HasPending())
            return Result<void>::Success();  // Application holds the actual module owners until its post-join drain.
        return m_gameplayRetirements->Close();
    }

    /** @copydoc EditorWorkspaceController::StartCameraCutPreview */
    Result<void> EditorWorkspaceController::StartCameraCutPreview(Cinematic::SequencePlaybackActivation activation,
                                                                  const Cinematic::CameraCutActivation &cuts,
                                                                  const Runtime::CameraViewContextId view,
                                                                  const Runtime::CameraTransitionSupport transitions,
                                                                  const std::optional<Runtime::CameraCrossFadeAdmission> crossFade) {
        const auto scene = m_runtimeScene.ActiveScene();
        if (!scene || !EligiblePreview(activation, m_playSession.IsActive()))
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        auto camera =
            Runtime::CameraService::Create({view, scene->RuntimeId(), Runtime::CameraViewDomain::EditorPreview}, transitions, crossFade);
        if (camera.HasError())
            return Result<void>::Failure(camera.ErrorValue());
        auto runtime = Cinematic::CinematicRuntimeService::Create({activation.player.handle.session});
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        auto candidate = std::make_unique<CameraPreviewState>(std::move(runtime).Value(), std::move(camera).Value());
        auto playback =
            Cinematic::CinematicCameraPlayback::Activate(candidate->runtime, candidate->camera, std::move(activation), cuts, *scene);
        if (playback.HasError())
            return Result<void>::Failure(playback.ErrorValue());
        candidate->playback.emplace(std::move(playback).Value());
        if (const auto started = candidate->runtime.Play(candidate->playback->Player()); started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        if (const auto published = candidate->playback->Publish(*scene); published.HasError())
            return published;
        m_cameraPreview = std::move(candidate);
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::AdvanceCameraCutPreview */
    Result<void> EditorWorkspaceController::AdvanceCameraCutPreview(const Cinematic::SequenceTime delta) {
        const auto scene = m_runtimeScene.ActiveScene();
        if (!m_cameraPreview || !scene)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::Closed));
        const Cinematic::SequenceFrameScratch scratch{{},
                                                      {},
                                                      m_cameraPreview->crossings,
                                                      m_cameraPreview->runtime.ServiceSnapshot().budget.maximumBoundaryOccurrences};
        if (const auto evaluated = m_cameraPreview->playback->Evaluate(delta, *scene, scratch, {}); evaluated.HasError()) {
            StopCameraCutPreview();
            return Result<void>::Failure(evaluated.ErrorValue());
        }
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::SeekCameraCutPreview */
    Result<void> EditorWorkspaceController::SeekCameraCutPreview(const Cinematic::SequenceTime position) {
        if (!m_cameraPreview)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::Closed));
        if (const auto seeked = m_cameraPreview->runtime.Seek(m_cameraPreview->playback->Player(), position); seeked.HasError())
            return Result<void>::Failure(seeked.ErrorValue());
        const auto scene = m_runtimeScene.ActiveScene();
        if (!scene) {
            StopCameraCutPreview();
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        }
        if (const auto published = m_cameraPreview->playback->Publish(*scene); published.HasError()) {
            StopCameraCutPreview();
            return published;
        }
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StopCameraCutPreview */
    void EditorWorkspaceController::StopCameraCutPreview() {
        m_cameraPreview.reset();
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
    }

    /** @copydoc EditorWorkspaceController::ResolveCameraCutPreview */
    Result<EditorWorkspaceController::CameraPreviewFrame> EditorWorkspaceController::ResolveCameraCutPreview(
        const Runtime::RuntimeSceneView scene, const std::optional<EditorViewportCrossFadeCapacity> capacity) {
        const auto &base = m_viewport.Current().camera;
        if (!m_cameraPreview)
            return Result<CameraPreviewFrame>::Success({base, std::nullopt});
        if (scene.RuntimeId() != m_cameraPreview->camera.Context().scene) {
            m_cameraPreview.reset();
            return Result<CameraPreviewFrame>::Success({base, std::nullopt});
        }
        if (capacity) {
            const auto admission = capacity->generation == 0
                                       ? std::nullopt
                                       : std::optional{Runtime::CameraCrossFadeAdmission{m_cameraPreview->camera.Context(),
                                                                                         capacity->generation, capacity->availableViews}};
            if (auto refreshed = m_cameraPreview->camera.RefreshCrossFadeAdmission(admission); refreshed.HasError())
                return Result<CameraPreviewFrame>::Failure(std::move(refreshed).ErrorValue());
        }
        if (auto published = m_cameraPreview->playback->Publish(scene); published.HasError()) {
            m_cameraPreview.reset();
            if (IsCameraTransitionRefusal(published.ErrorValue())) {
                m_activeRuntimeRevision = {};
                return Result<CameraPreviewFrame>::Failure(std::move(published).ErrorValue());
            }
            LOG_ERROR("editor.viewport", "Camera preview released after binding loss: %s", published.ErrorValue().message.c_str());
            return Result<CameraPreviewFrame>::Success({base, std::nullopt});
        }
        if (m_cameraPreview->frame == std::numeric_limits<std::uint64_t>::max())
            return Result<CameraPreviewFrame>::Failure(MakeError(Runtime::CameraErrors::InvalidFrame));
        const auto selection =
            m_cameraPreview->camera.Commit(++m_cameraPreview->frame, scene, AuthoringProposal(base, scene.StructuralRevision()));
        if (selection.HasError())
            return Result<CameraPreviewFrame>::Failure(selection.ErrorValue());
        CameraPreviewFrame frame{ViewportCamera(selection.Value().selected.values), std::nullopt};
        if (const auto &source = selection.Value().crossFade)
            frame.crossFade.emplace(source->source, source->destinationWeight, source->admissionGeneration);
        return Result<CameraPreviewFrame>::Success(frame);
    }
}  // namespace Horo::Editor
