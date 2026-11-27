#include "EditorMountedWorldViewport.h"

#include "EditorProjectSceneHostOwner.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Application::Internal {
    /** @copydoc EditorMountedWorldViewport::Publish */
    Result<void> EditorMountedWorldViewport::Publish(EditorMountedWorldViewportView value) {
        if (current_.owner.IsValid() || !value.owner.IsValid() || !value.view.IsValid() || !value.packet)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Invalid));
        const auto scene = value.packet->Scene();
        if (scene.HasError())
            return Result<void>::Failure(scene.ErrorValue());
        if (scene.Value().lightSceneDomain != value.owner.value)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        current_ = std::move(value);
        return Result<void>::Success();
    }

    /** @copydoc EditorMountedWorldViewport::Replace */
    Result<void> EditorMountedWorldViewport::Replace(const std::shared_ptr<const MountedWorldRenderPacket> &expected,
                                                     std::shared_ptr<const MountedWorldRenderPacket> candidate) {
        using WorldStreaming::WorldArtifactErrors::Stale;
        if (!expected || current_.packet != expected || !candidate || !candidate->IsSuccessorOf(*expected))
            return Result<void>::Failure(MakeError(Stale));
        const auto scene = candidate->Scene();
        if (scene.HasError())
            return Result<void>::Failure(scene.ErrorValue());
        if (scene.Value().lightSceneDomain != current_.owner.value)
            return Result<void>::Failure(MakeError(Stale));
        current_.packet = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc EditorMountedWorldViewport::Withdraw */
    void EditorMountedWorldViewport::Withdraw(const Runtime::SceneRuntimeId owner) noexcept {
        if (current_.owner == owner)
            current_ = {};
    }
}  // namespace Horo::Application::Internal

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc ProjectPlaySceneHost::PrepareWorldRenderer */
    Result<std::unique_ptr<Render::RenderOriginSceneAdapter>> ProjectPlaySceneHost::PrepareWorldRenderer(
        const Render::RenderViewId view) const {
        using namespace WorldStreaming;
        const auto participant = OriginRebaseParticipantId::Create(identity_.value);
        if (participant.HasError())
            return Result<std::unique_ptr<Render::RenderOriginSceneAdapter>>::Failure(participant.ErrorValue());
        // Desktop may retain the last prepared frame beside the current publication while capturing its successor.
        const std::size_t slots = selectedXrHost_ ? 1 : 3;
        return Render::RenderOriginSceneAdapter::Create(mountedWorld_->Origin(), view,
                                                        {participant.Value(), OriginRebaseParticipantRevision::Create(1).Value()},
                                                        {slots, std::max<std::size_t>(1, worldViewport_->meshes.size()), 1});
    }

    /** @copydoc ProjectPlaySceneHost::QueueMountedXrScene */
    Result<void> ProjectPlaySceneHost::QueueMountedXrScene(const Runtime::RuntimeSceneDefinition &definition,
                                                           const MountedWorldStartup &boundary) {
        const auto view = worldViewport_->issueView();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        auto renderer = PrepareWorldRenderer(view.Value());
        if (renderer.HasError())
            return Result<void>::Failure(renderer.ErrorValue());
        worldRenderer_ = std::move(renderer).Value();
        return mountedWorld_->QueueInitialCell(definition, *worldSelection_->initialCell, boundary,
                                               resources_.registry_.Snapshot().Revision());
    }

    /** @copydoc ProjectPlaySceneHost::CaptureMountedXrGeometry */
    Result<Editor::EditorMountedXrScene> ProjectPlaySceneHost::CaptureMountedXrGeometry(const Runtime::RuntimeSceneView &candidate,
                                                                                        const Runtime::SceneObjectId trackingRoot) {
        using Output = Result<Editor::EditorMountedXrScene>;
        if (!mountedWorld_ || !worldRenderer_ || !worldViewport_ || !viewportPrimitives_)
            return Output::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        const auto placement = mountedWorld_->ResolveInitialTrackingRoot(candidate, trackingRoot);
        if (placement.HasError())
            return Output::Failure(placement.ErrorValue());
        auto captured = mountedWorld_->CaptureInitialGeometry(candidate, worldViewport_->camera, worldViewport_->meshes,
                                                              *viewportPrimitives_, *worldRenderer_);
        if (captured.HasError())
            return Output::Failure(captured.ErrorValue());

        struct RetainedGeometry final {
            MountedWorldRenderPacket packet;
            std::optional<Render::RenderOriginLocalScene> view;
        };

        auto owner = std::make_shared<RetainedGeometry>(RetainedGeometry{std::move(captured).Value(), {}});
        auto view = owner->packet.OriginSceneWithOwners();
        if (view.HasError())
            return Output::Failure(view.ErrorValue());
        const auto &[geometry, drawEntities] = view.Value();
        const auto geometryOrigin = geometry.origin.Get();
        const auto placementOrigin = placement.Value().origin.Get();
        if (geometryOrigin.HasError() || placementOrigin.HasError() || geometryOrigin.Value() != placementOrigin.Value())
            return Output::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        owner->view.emplace(geometry);
        return Output::Success({{owner, &*owner->view}, trackingRoot, placement.Value().originFromLocal, drawEntities});
    }

    /** @copydoc ProjectPlaySceneHost::CompleteWorldStartup */
    Result<void> ProjectPlaySceneHost::CompleteWorldStartup(const Runtime::FrameContext &frame) {
        if (selectedXrHost_ && mountedWorld_) {
            const auto active = mountedWorld_->ReconcileInitialCell();
            return active.HasError() ? Result<void>::Failure(active.ErrorValue()) : Result<void>::Success();
        }
        if (!worldStartup_ || !scenes_->ActiveScene())
            return Result<void>::Success();
        if (frame.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Cancelled));

        const auto &viewport = *worldViewport_;
        auto view = viewport.issueView();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        auto renderer = PrepareWorldRenderer(view.Value());
        if (renderer.HasError())
            return Result<void>::Failure(renderer.ErrorValue());
        if (auto active = mountedWorld_->ActivateInitialCell(*worldSelection_->initialCell, *worldStartup_, &frame); active.HasError())
            return active;
        const auto parent = scenes_->ActiveScene();
        if (!parent)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        auto packet = CaptureDesktopWorldViewport(*parent, *renderer.Value());
        if (packet.HasError())
            return Result<void>::Failure(packet.ErrorValue());
        if (auto published = viewport.publication->Publish({identity_, view.Value(), std::move(packet).Value()}); published.HasError())
            return published;
        worldRenderer_ = std::move(renderer).Value();
        worldStartup_.reset();
        return Result<void>::Success();
    }

    /** @copydoc ProjectPlaySceneHost::CaptureDesktopWorldViewport */
    Result<std::shared_ptr<const MountedWorldRenderPacket>> ProjectPlaySceneHost::CaptureDesktopWorldViewport(
        const Runtime::RuntimeSceneView &parent, Render::RenderOriginSceneAdapter &renderer) {
        using Output = Result<std::shared_ptr<const MountedWorldRenderPacket>>;
        const auto &viewport = *worldViewport_;
        auto packet = mountedWorld_->CaptureGeometry(parent, viewport.camera, viewport.meshes, *viewportPrimitives_, renderer);
        if (packet.HasError())
            return Output::Failure(std::move(packet).ErrorValue());
        return Output::Success(std::make_shared<const MountedWorldRenderPacket>(std::move(packet).Value()));
    }

    /** @copydoc ProjectPlaySceneHost::RefreshDesktopWorldViewport */
    Result<void> ProjectPlaySceneHost::RefreshDesktopWorldViewport(const Runtime::FrameContext &frame) {
        using namespace WorldStreaming;
        if (!UsesMountedViewport() || !worldRenderer_)
            return Result<void>::Success();
        auto &publication = *worldViewport_->publication;
        const auto current = publication.Current();
        if (current.owner != identity_ || !current.packet)
            return Result<void>::Failure(MakeError(WorldArtifactErrors::Stale));
        if (current.packet->Scene().HasValue())
            return Result<void>::Success();
        if (frame.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WorldArtifactErrors::Cancelled));
        const auto scene = scenes_->ActiveScene();
        if (!scene)
            return Result<void>::Failure(MakeError(WorldArtifactErrors::Stale));
        auto replacement = CaptureDesktopWorldViewport(*scene, *worldRenderer_);
        if (replacement.HasError())
            return Result<void>::Failure(std::move(replacement).ErrorValue());
        return publication.Replace(current.packet, std::move(replacement).Value());
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
