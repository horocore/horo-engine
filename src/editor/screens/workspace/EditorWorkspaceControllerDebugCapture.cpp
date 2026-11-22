#include "EditorInspectionCommandEndpoint.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor CaptureUnavailable{.domain = ErrorDomainId{"editor.debug_capture"},
                                                     .code = ErrorCode{"capture_unavailable"},
                                                     .defaultSeverity = ErrorSeverity::Warning,
                                                     .summary = "The exact visible Play target is unavailable."};
        const ErrorCodeDescriptor CaptureBusy{.domain = ErrorDomainId{"editor.debug_capture"},
                                              .code = ErrorCode{"capture_busy"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "A capture is already running or its start rate is limited."};
        const ErrorCodeDescriptor ImageUnavailable{.domain = ErrorDomainId{"editor.debug_capture"},
                                                   .code = ErrorCode{"image_unavailable"},
                                                   .defaultSeverity = ErrorSeverity::Warning,
                                                   .summary = "The exact authored viewport is unavailable."};
    }  // namespace

    /** @copydoc EditorWorkspaceController::BindDebugCapture */
    std::weak_ptr<const EditorDebugCaptureEndpoint> EditorWorkspaceController::BindDebugCapture(std::string project,
                                                                                                std::uint64_t authorizationRevision,
                                                                                                IEditorViewportRenderer *viewport) {
        m_debugCapture.Clear();
        m_debugCaptureAuthorized = {};
        m_debugCaptureEntity.reset();
        EditorDebugCaptureEndpoint endpoint{.project = std::move(project),
                                            .authorizationRevision = authorizationRevision,
                                            .owner = std::this_thread::get_id(),
                                            .start =
                                                [this](const EditorDebugCaptureRequest &request, std::function<bool()> authorized) {
            return StartDebugCapture(request, std::move(authorized));
        },
                                            .read =
                                                [this](std::uint64_t id) {
            return m_debugCapture.Read(id, EditorDebugCaptureStorage::Clock::now());
        },
                                            .cancel = [this](std::uint64_t id) {
            return m_debugCapture.Cancel(id, EditorDebugCaptureStorage::Clock::now());
        }};
        if (viewport) {
            endpoint.startImage = [this, viewport](EditorViewportImageRegion region, std::function<bool()> authorized) {
                return StartDebugImage(*viewport, region, std::move(authorized));
            };
            endpoint.readImage = [viewport](std::uint64_t id) {
                return viewport->ReadImageCapture(id);
            };
            endpoint.cancelImage = [viewport](std::uint64_t id) {
                return viewport->CancelImageCapture(id);
            };
        }
        m_debugCaptureEndpoint = std::make_shared<const EditorDebugCaptureEndpoint>(std::move(endpoint));
        return m_debugCaptureEndpoint;
    }

    /** @copydoc EditorWorkspaceController::StartDebugImage */
    Result<std::uint64_t> EditorWorkspaceController::StartDebugImage(IEditorViewportRenderer &viewport, EditorViewportImageRegion region,
                                                                     std::function<bool()> authorized) {
        const auto expected = InspectSceneRevision();
        const auto active = m_runtimeScene.ActiveScene();
        if (!expected.HasValue() || !active || !HasCurrentInspectionGeometry(*active))
            return Result<std::uint64_t>::Failure(MakeError(ImageUnavailable));
        auto valid = [this, lease = std::weak_ptr{m_debugCaptureEndpoint}, scene = expected.Value(), runtime = active->RuntimeId(),
                      authorized = std::move(authorized)] {
            return !lease.expired() && authorized && authorized() && IsCurrentDebugImageSource(scene, runtime);
        };
        if (!valid())
            return Result<std::uint64_t>::Failure(MakeError(ImageUnavailable));
        return viewport.RequestImageCapture(region, std::move(valid));
    }

    /** @copydoc EditorWorkspaceController::IsCurrentDebugImageSource */
    bool EditorWorkspaceController::IsCurrentDebugImageSource(InspectionSceneRevision revision, Runtime::SceneRuntimeId runtime) const {
        if (m_playSession.State() != EditorPlaySessionState::Idle || m_cameraPreview || m_activeTerrainDocument)
            return false;
        const auto current = InspectSceneRevision();
        const auto active = m_runtimeScene.ActiveScene();
        return current.HasValue() && current.Value() == revision && active && active->RuntimeId() == runtime &&
               HasCurrentInspectionGeometry(*active);
    }

    /** @copydoc EditorWorkspaceController::StartDebugCapture */
    Result<std::uint64_t> EditorWorkspaceController::StartDebugCapture(const EditorDebugCaptureRequest &request,
                                                                       std::function<bool()> authorized) {
        using Output = Result<std::uint64_t>;
        const auto scene = m_playSession.SceneView();
        if (!authorized || !authorized() || m_playSession.State() != EditorPlaySessionState::Playing || !scene || !scene->IsCurrent())
            return Output::Failure(MakeError(CaptureUnavailable));
        const SceneObjectId object{request.target};
        const auto authored = std::ranges::find(m_viewModel.objects, object, &SceneObject::id);
        if (authored == m_viewModel.objects.end() || !authored->effectivelyVisible)
            return Output::Failure(MakeError(CaptureUnavailable));
        const auto entity = scene->Find({request.target});
        if (!entity)
            return Output::Failure(MakeError(CaptureUnavailable));
        const auto id = m_debugCapture.Start(request, EditorDebugCaptureStorage::Clock::now());
        if (!id)
            return Output::Failure(MakeError(CaptureBusy));
        m_debugCaptureScene = scene->RuntimeId();
        m_debugCaptureAuthorized = std::move(authorized);
        m_debugCaptureEntity = entity;
        m_debugCaptureObject = object;
        return Output::Success(*id);
    }

    /** @copydoc EditorWorkspaceController::UpdateDebugCapture */
    void EditorWorkspaceController::UpdateDebugCapture() {
        if (!m_debugCaptureEntity)
            return;
        if (!m_debugCaptureAuthorized || !m_debugCaptureAuthorized()) {
            m_debugCapture.Clear();
            m_debugCaptureAuthorized = {};
            m_debugCaptureEntity.reset();
            return;
        }
        const auto scene = m_playSession.SceneView();
        if (m_playSession.State() != EditorPlaySessionState::Playing || !scene || !scene->IsCurrent() ||
            scene->RuntimeId() != m_debugCaptureScene || scene->Find({m_debugCaptureObject.value}) != m_debugCaptureEntity) {
            m_debugCapture.RetireScene();
            m_debugCaptureEntity.reset();
            return;
        }
        const auto now = EditorDebugCaptureStorage::Clock::now();
        if (!m_debugCapture.Due(now))
            return;
        const auto pose = ResolveSceneObjectWorldTransforms(*scene, m_debugCaptureObject);
        if (pose.HasError()) {
            m_debugCapture.RetireScene();
            return;
        }
        const auto point = Math::TransformPoint(pose.Value().localToWorld, {});
        if (!Math::IsFinite(point)) {
            m_debugCapture.RetireScene();
            return;
        }
        m_debugCapture.Sample({point.x, point.y, point.z}, now);
    }
}  // namespace Horo::Editor
