#include "../commands/EditorInspectionCommandEndpoint.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <utility>

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

        /** @brief Keeps one owner-lane capture identity until the viewport or endpoint retires. */
        template <typename Start>
        void BindImageReadback(EditorDebugCaptureEndpoint &endpoint, IEditorViewportRenderer &viewport, CancellationToken lifetime,
                               Start start) {
            const auto activeImage = std::make_shared<std::optional<std::uint64_t>>();
            endpoint.startImage = [lifetime, activeImage, start = std::move(start)](EditorDebugImageSource source,
                                                                                    EditorViewportImageRegion region,
                                                                                    std::function<bool()> authorized) {
                if (lifetime.IsCancellationRequested())
                    return Result<std::uint64_t>::Failure(MakeError(ImageUnavailable));
                auto admitted = start(source, region, std::move(authorized));
                if (admitted.HasValue())
                    activeImage->emplace(admitted.Value());
                return admitted;
            };
            endpoint.retireImage = [&viewport, lifetime, activeImage]() noexcept {
                if (const auto id = std::exchange(*activeImage, std::nullopt); id && !lifetime.IsCancellationRequested())
                    static_cast<void>(viewport.CancelImageCapture(*id));
            };
            endpoint.readImage = [&viewport, lifetime](std::uint64_t id) {
                if (lifetime.IsCancellationRequested())
                    return std::optional<EditorViewportImageSnapshot>{};
                return viewport.ReadImageCapture(id);
            };
            endpoint.cancelImage = [&viewport, lifetime](std::uint64_t id) {
                return !lifetime.IsCancellationRequested() && viewport.CancelImageCapture(id);
            };
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::RetireDebugCaptureBinding */
    void EditorWorkspaceController::RetireDebugCaptureBinding() noexcept {
        if (m_debugCaptureEndpoint && m_debugCaptureEndpoint->retireImage)
            m_debugCaptureEndpoint->retireImage();
        m_debugCaptureLifetime.RequestCancellation();
        m_debugCaptureEndpoint.reset();
        m_debugCapture.Clear();
        m_debugCaptureAuthorized = {};
        m_debugCaptureEntity.reset();
    }

    /** @copydoc EditorWorkspaceController::BindDebugCapture */
    std::weak_ptr<const EditorDebugCaptureEndpoint> EditorWorkspaceController::BindDebugCapture(std::string project,
                                                                                                std::uint64_t authorizationRevision,
                                                                                                IEditorViewportRenderer *viewport) {
        RetireDebugCaptureBinding();
        m_debugCaptureLifetime = CancellationSource{};
        const auto lifetime = m_debugCaptureLifetime.Token();
        EditorDebugCaptureEndpoint endpoint{.project = std::move(project),
                                            .authorizationRevision = authorizationRevision,
                                            .owner = std::this_thread::get_id(),
                                            .start =
                                                [this, lifetime](const EditorDebugCaptureRequest &request,
                                                                 std::function<bool()> authorized) {
            if (lifetime.IsCancellationRequested())
                return Result<std::uint64_t>::Failure(MakeError(CaptureUnavailable));
            return StartDebugCapture(request, std::move(authorized));
        },
                                            .read =
                                                [this, lifetime](std::uint64_t id) {
            if (lifetime.IsCancellationRequested() || !m_debugCaptureAuthorized || !m_debugCaptureAuthorized())
                return std::optional<EditorDebugCaptureSnapshot>{};
            return m_debugCapture.Read(id, EditorDebugCaptureStorage::Clock::now());
        },
                                            .cancel =
                                                [this, lifetime](std::uint64_t id) {
            return !lifetime.IsCancellationRequested() && m_debugCapture.Cancel(id, EditorDebugCaptureStorage::Clock::now());
        },
                                            .lifetime = lifetime};
        if (viewport) {
            endpoint.readImageSource = [this, viewport, lifetime] {
                if (lifetime.IsCancellationRequested())
                    return std::optional<EditorDebugImageSource>{};
                return ReadDebugImageSource(*viewport);
            };
            BindImageReadback(endpoint, *viewport, lifetime,
                              [this, viewport](EditorDebugImageSource source, EditorViewportImageRegion region,
                                               std::function<bool()> authorized) {
                return StartDebugImage(*viewport, source, region, std::move(authorized));
            });
        }
        m_debugCaptureEndpoint = std::make_shared<const EditorDebugCaptureEndpoint>(std::move(endpoint));
        return m_debugCaptureEndpoint;
    }

    /** @copydoc EditorWorkspaceController::StartDebugImage */
    Result<std::uint64_t> EditorWorkspaceController::StartDebugImage(IEditorViewportRenderer &viewport, EditorDebugImageSource expected,
                                                                     EditorViewportImageRegion region, std::function<bool()> authorized) {
        if (!region.Fits(expected.width, expected.height))
            return Result<std::uint64_t>::Failure(MakeError(ImageUnavailable));
        auto valid = [this, &viewport, lifetime = m_debugCaptureLifetime.Token(), expected, authorized = std::move(authorized)] {
            return !lifetime.IsCancellationRequested() && authorized && authorized() && ReadDebugImageSource(viewport) == expected;
        };
        if (!valid())
            return Result<std::uint64_t>::Failure(MakeError(ImageUnavailable));
        return viewport.RequestImageCapture(region, std::move(valid));
    }

    /** @copydoc EditorWorkspaceController::ReadDebugImageSource */
    std::optional<EditorDebugImageSource> EditorWorkspaceController::ReadDebugImageSource(const IEditorViewportRenderer &viewport) const {
        if (m_playSession.State() != EditorPlaySessionState::Idle || m_cameraPreview || m_activeTerrainDocument)
            return std::nullopt;
        const auto scene = InspectSceneRevision();
        const auto active = m_runtimeScene.ActiveScene();
        const auto extent = viewport.RequestedExtent();
        if (!scene.HasValue() || !active || !HasCurrentInspectionGeometry(*active) || !extent.IsValid())
            return std::nullopt;
        return EditorDebugImageSource{scene.Value().epoch,     scene.Value().revision, m_viewport.Current().revision.value,
                                      m_viewportSceneRevision, extent.width,           extent.height};
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
