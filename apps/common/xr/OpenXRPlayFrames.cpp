#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/XR/XRMultiviewScene.h"
#include "OpenXRFrameCompositor.h"
#include "OpenXRPlayFrameState.h"
#include "OpenXRPlayHost.h"
#include "OpenXRVulkanSwapchain.h"

#include <bit>
#include <cmath>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    namespace {
        /** @brief Maps one actual ordered native eye observation into existing typed view evidence; no identity is issued. */
        XRViewDescriptor DescribeEye(const NativePresentationViews &observed, const XRSessionId &session, std::uint32_t eye,
                                     const OpenXRApplicationProjection &projection) {
            const auto &native = observed.views[eye];
            XRViewDescriptor view;
            view.id = {session, {.index = eye, .generation = observed.eyes[eye].id.slot.generation}};
            view.order = eye;
            view.role = eye == 0 ? XRViewRole::PrimaryLeft : XRViewRole::PrimaryRight;
            view.pose = {session,
                         observed.eyes[eye],
                         observed.reference,
                         XRPosePurpose::PresentationPrediction,
                         observed.referenceToWorld.time,
                         {}};
            auto &components = view.pose.components;
            const bool position = (observed.flags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
            const bool orientation = (observed.flags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
            const bool trackedPosition = (observed.flags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
            const bool trackedOrientation = (observed.flags & XR_VIEW_STATE_ORIENTATION_TRACKED_BIT) != 0;
            if (position)
                components.positionMeters = {{Math::Vec3{native.pose.position.x, native.pose.position.y, native.pose.position.z}},
                                             trackedPosition ? XRPoseComponentValidity::Tracked : XRPoseComponentValidity::Inferred};
            if (orientation)
                components.orientation = {{Math::Quaternion{native.pose.orientation.x, native.pose.orientation.y, native.pose.orientation.z,
                                                            native.pose.orientation.w}},
                                          trackedOrientation ? XRPoseComponentValidity::Tracked : XRPoseComponentValidity::Inferred};
            components.confidence = XRTrackingConfidence::Low;
            if (!position && !orientation)
                components.confidence = XRTrackingConfidence::None;
            else if (trackedPosition && trackedOrientation)
                components.confidence = XRTrackingConfidence::High;
            components.loss = XRTrackingLossState::FullyLost;
            if (position && orientation)
                components.loss = XRTrackingLossState::None;
            else if (position)
                components.loss = XRTrackingLossState::OrientationLost;
            else if (orientation)
                components.loss = XRTrackingLossState::PositionLost;
            view.projection = {std::tan(native.fov.angleLeft), std::tan(native.fov.angleRight), std::tan(native.fov.angleDown),
                               std::tan(native.fov.angleUp)};
            view.recommendedExtent = projection.recommended[eye];
            view.maximumExtent = projection.maximum[eye];
            return view;
        }

        /** @brief Describes the exact acquired color subresource for one ordered eye without issuing another target. */
        XRExternalRenderTargetDescriptor DescribeEyeTarget(const XRViewDescriptor &view, const NativeSwapchainAcquisition &acquisition,
                                                           const Render::RenderTextureDescriptor &allocation, std::uint32_t eye) {
            return {view.id,
                    XRExternalRenderTargetRole::ProjectionColor,
                    acquisition.image.target,
                    acquisition.image,
                    allocation.format,
                    allocation.usage,
                    allocation.extent,
                    {0, 0, allocation.extent.width, allocation.extent.height},
                    eye,
                    2,
                    1};
        }
    }  // namespace

    /** @copydoc OpenXRPlayHost::PrepareFrameResources */
    Result<void> OpenXRPlayHost::PrepareFrameResources(const XRSessionId &candidate, const Render::RenderTextureDescriptor &color) {
        if (render_ || !candidate.IsValid() || frames_.Snapshot().phase != XRFramePhase::Idle)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        auto graphics = composition_.session.BorrowGraphics(candidate);
        if (graphics.HasError())
            return Result<void>::Failure(graphics.ErrorValue());
        auto binding = graphics.Value()->BorrowBinding(candidate);
        if (binding.HasError())
            return Result<void>::Failure(binding.ErrorValue());
        auto device = Editor::EditorVulkanXrViewportAccess::Device(composition_.viewport);
        if (device.HasError())
            return Result<void>::Failure(device.ErrorValue());
        const auto &selected = device.Value();
        if (!selected.deviceResolver || selected.device != binding.Value()->device || selected.instance != binding.Value()->instance ||
            selected.physical != binding.Value()->physicalDevice || selected.graphicsFamily != binding.Value()->queueFamilyIndex)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        // Device functions are resolved ONLY through the actual selected device resolver.
        const Render::Detail::VulkanExternalViewDispatch views{std::bit_cast<PFN_vkCreateImageView>(
                                                                   selected.deviceResolver(selected.device, "vkCreateImageView")),
                                                               std::bit_cast<PFN_vkDestroyImageView>(
                                                                   selected.deviceResolver(selected.device, "vkDestroyImageView"))};
        if (!views.create || !views.destroy)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        render_ = std::make_shared<FrameState>(*this, candidate, *graphics.Value(), *binding.Value(), selected, views, color);
        if (auto bound = render_->images.Bind(render_->color); bound.HasError())
            return bound;
        // This is the single actual color allocation for this issued session. The session generation
        // fences its sole target slot; the swapchain owner independently issues every acquisition generation.
        return render_->color.Create({{candidate, {.index = 0, .generation = 1}}, color}, getProc_);
    }

    /** @copydoc OpenXRPlayHost::FinishFrameResources */
    Result<void> OpenXRPlayHost::FinishFrameResources(const XRSessionId &candidate, const XRCapabilitySnapshot &capabilities) {
        if (!render_ || render_->session != candidate || sessions_.Snapshot().session != candidate || events_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto configured = frames_.BindConfiguration({candidate, {.index = 0, .generation = 1}}, capabilities,
                                                              {.maximumViews = 2, .maximumImages = 1, .maximumLayers = 1});
            configured != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto bound = render_->compositor.Bind({getProc_}, {}); bound.HasError())
            return bound;
        if (auto bound = clock_.BindNative({getProc_}); bound.HasError())
            return bound;
        render_->clockBound = true;
        events_.emplace(native_, sessions_, frames_);
        const std::array owners{&render_->color};
        if (auto bound = events_->Bind({getProc_}, owners); bound.HasError())
            return bound;
        eventsBound_ = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::UsesXrScene */
    bool OpenXRPlayHost::UsesXrScene() const noexcept {
        return scene_ && scene_->published && !scene_->retiring;
    }

    /** @copydoc OpenXRPlayHost::PresentationState */
    OpenXRPlayPresentation OpenXRPlayHost::PresentationState() const noexcept {
        using enum Horo::Application::Internal::OpenXRPlayPresentation;
        if (std::this_thread::get_id() != lane_ || busy_)
            return Pending;
        if (!scene_ && !render_)
            return Inactive;
        return UsesXrScene() && sessions_.Snapshot().admitsFrames && frames_.Snapshot().phase == XRFramePhase::Idle ? FrameAdmitted
                                                                                                                    : Pending;
    }

    /** @copydoc OpenXRPlayHost::AbortRenderFrame */
    Result<void> OpenXRPlayHost::AbortRenderFrame() {
        using enum Horo::XR::XRFramePhase;
        if (!render_ || frames_.Snapshot().phase == Idle)
            return Result<void>::Success();
        render_->rendered = false;
        render_->guiFinished = true;
        if (const auto state = frames_.Snapshot(); state.renderAdmitted && state.phase != Aborting && state.phase != ImagesReleased &&
                                                   frames_.Abort(state.frame) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::PrepareRenderFrame */
    Result<bool> OpenXRPlayHost::PrepareRenderFrame(const std::uint64_t frameNumber) {
        using Output = Result<bool>;
        if (auto lane = Lane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        const Operation operation{*this};
        if (!UsesXrScene() || !render_ || !eventsBound_ || eventsPending_)
            return Output::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto valid = scene_->Validate(*this); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (auto tail = ProgressTail(); tail.HasError())
            return Output::Failure(tail.ErrorValue());
        auto state = frames_.Snapshot();
        if (state.phase != XRFramePhase::Idle && (render_->submitted || render_->guiFinished))
            return Output::Success(false);
        if (state.phase == XRFramePhase::Idle) {
            const auto started = render_->StartPrediction();
            if (started.HasError() || !started.Value())
                return started;
        }
        if (const auto pacing = render_->CheckDeadline(); pacing.HasError() || !pacing.Value())
            return pacing;
        if (frames_.Snapshot().phase == XRFramePhase::Begun) {
            if (auto located = render_->LocateViews(); located.HasError())
                return render_->Fail(located.ErrorValue());
        }
        const auto acquired = render_->AcquireEyeImages();
        if (acquired.HasError())
            return render_->Fail(acquired.ErrorValue());
        if (!acquired.Value())
            return Output::Success(false);
        if (auto bound = BindRenderFrame(frameNumber); bound.HasError())
            return render_->Fail(bound.ErrorValue());
        return Output::Success(true);
    }

    /** @copydoc OpenXRPlayHost::BindRenderFrame */
    Result<void> OpenXRPlayHost::BindRenderFrame(const std::uint64_t frameNumber) {
        if (!scene_ || !scene_->projection || !render_->views || !render_->acquisition)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto frame = frames_.Snapshot();
        const auto &observed = *render_->views;
        const auto &support = composition_.frontend.Capabilities().support;
        const auto revision = XRViewConfigurationRevision::Create(scene_->projection->capabilities.Revision().Value());
        if (revision.HasError())
            return Result<void>::Failure(revision.ErrorValue());
        const XRViewConfigurationId configuration{render_->session, {.index = 0, .generation = 1}};
        std::array<XRViewDescriptor, 2> views{};
        std::array<XRExternalRenderTargetDescriptor, 2> targets{};
        for (std::uint32_t eye = 0; eye < views.size(); ++eye) {
            views[eye] = DescribeEye(observed, render_->session, eye, *scene_->projection);
            const auto &view = views[eye];
            targets[eye] = DescribeEyeTarget(view, *render_->acquisition, render_->allocation, eye);
        }
        const std::array images{render_->acquisition->image};
        const XRViewConfigurationDescriptor config{CurrentXRContractVersion,
                                                   render_->session,
                                                   configuration,
                                                   revision.Value(),
                                                   XRViewConfigurationType::PrimaryStereo,
                                                   XREnvironmentBlendMode::Opaque,
                                                   2};
        auto plan = XRViewRenderPlan::Create({config, frame.predictedDisplayTime, views, targets, images}, render_->session, configuration,
                                             revision.Value(), input_.origin, 2, {});
        if (plan.HasError())
            return Result<void>::Failure(plan.ErrorValue());
        const Render::MultiviewPublication publication{scene_->scene.value,       frameNumber,
                                                       input_.origin.Value(),     revision.Value().Value(),
                                                       support.deviceIncarnation, support.capabilityRevision};
        auto converted = render_->ConvertScene(plan.Value(), configuration, revision.Value(), input_.origin, publication);
        if (converted.HasError())
            return Result<void>::Failure(converted.ErrorValue());
        if (auto bound =
                Editor::EditorVulkanXrViewportAccess::Bind(composition_.viewport, render_->acquisition->texture, render_->eyeViews,
                                                           publication, converted.Value().Scene().Views(), scene_->geometry->Instances());
            bound.HasError())
            return bound;
        render_->plan.emplace(std::move(plan).Value());
        render_->hostFrameNumber = frameNumber;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ExecuteRenderFrame */
    Result<void> OpenXRPlayHost::ExecuteRenderFrame(Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!UsesXrScene() || !render_ || !render_->plan || !render_->acquisition || render_->submitted)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{*this};
        if (auto executed = Editor::EditorVulkanXrViewportAccess::Execute(composition_.viewport, frame, output); executed.HasError()) {
            const auto aborted = AbortRenderFrame();
            (void)aborted;
            return executed;
        }
        if (const std::array images{render_->acquisition->image}; frames_.Submit(frames_.Snapshot().frame, images) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        render_->submitted = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::MirrorView */
    Editor::EditorViewportTextureView OpenXRPlayHost::MirrorView(const std::uint64_t frameNumber) const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || !UsesXrScene() || !render_ || !render_->submitted || render_->guiFinished ||
            !render_->plan || !render_->acquisition || render_->hostFrameNumber != frameNumber ||
            sessions_.Snapshot().state == XRSessionState::Lost)
            return {};
        const auto mirror = Editor::EditorVulkanXrViewportAccess::Mirror(composition_.viewport, frameNumber, render_->eyeViews[0]);
        return mirror.IsValid() && mirror.sampledView.IsValid() && mirror.sampledView == render_->eyeViews[0]
                   ? mirror
                   : Editor::EditorViewportTextureView{};
    }

    /** @copydoc OpenXRPlayHost::FinishGui */
    Result<void> OpenXRPlayHost::FinishGui(const bool accepted) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!render_ || render_->guiFinished || frames_.Snapshot().phase == XRFramePhase::Idle || (accepted && !render_->submitted))
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        render_->guiFinished = true;
        render_->rendered = accepted && render_->submitted && UsesXrScene();
        return render_->rendered ? Result<void>::Success() : AbortRenderFrame();
    }

    /** @copydoc OpenXRPlayHost::ProgressRenderTail */
    Result<void> OpenXRPlayHost::ProgressRenderTail() {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        const Operation operation{*this};
        return ProgressTail();
    }

    /** @copydoc OpenXRPlayHost::ProgressTail */
    Result<void> OpenXRPlayHost::ProgressTail() {
        if (!render_ || frames_.Snapshot().phase == XRFramePhase::Idle)
            return Result<void>::Success();
        if (sessions_.Snapshot().state == XRSessionState::Lost)
            return Result<void>::Success();  // Explicit destructive retirement owns this debt; ordinary End is no longer admitted.
        if (!render_->guiFinished && !(scene_ && scene_->retiring))
            return Result<void>::Success();
        if (scene_ && scene_->retiring) {
            auto aborted = AbortRenderFrame();
            if (aborted.HasError())
                return aborted;
        }
        const auto state = frames_.Snapshot();
        if (render_->submitted) {
            const auto polled = Editor::EditorVulkanXrViewportAccess::Poll(composition_.viewport);
            if (polled.HasError())
                return polled;
        }
        const auto retired = render_->RetireImageReferences();
        if (retired.HasError())
            return Result<void>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<void>::Success();
        if (render_->acquisitionRecorded && render_->acquisition && state.phase != XRFramePhase::ImagesReleased &&
            frames_.Release(state.frame, render_->acquisition->image) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        Result<void> ended = Result<void>::Success();
        if (render_->rendered && render_->plan && render_->acquisition) {
            const std::array targets{NativeProjectionTarget{&render_->color, render_->acquisition->image}};
            ended = render_->compositor.EndProjection(state.frame, *render_->plan, targets);
        } else {
            ended = render_->compositor.EndEmpty(state.frame);
        }
        if (ended.HasError())
            return ended;
        render_->acquisition.reset();
        render_->plan.reset();
        render_->views.reset();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireViewsAfterLoss */
    Result<void> OpenXRPlayHost::FrameState::RetireViewsAfterLoss() {
        // Loss closes canonical admission, not GPU work. Preserve the same queue references and retry debt.
        if (submitted) {
            if (auto polled = Editor::EditorVulkanXrViewportAccess::Poll(host.composition_.viewport); polled.HasError())
                return polled;
        }
        for (auto &view : eyeViews) {
            if (!view.IsValid())
                continue;
            if (auto released = host.composition_.frontend.ReleaseTextureView(view); released.HasError())
                return released;
            view = {};
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireResources */
    Result<bool> OpenXRPlayHost::FrameState::RetireResources(OpenXRPlayHost &host, const XRSessionId &candidate) {
        using Output = Result<bool>;
        if (host.render_ && host.render_->session != candidate)
            return Output::Failure(MakeError(XRErrors::IdentityStale));
        const bool lost = host.sessions_.Snapshot().state == XRSessionState::Lost;
        if (!lost) {
            if (auto tail = host.ProgressTail(); tail.HasError())
                return Output::Failure(tail.ErrorValue());
            if (host.frames_.Snapshot().phase != XRFramePhase::Idle)
                return Output::Success(false);  // Normal stopping must finish the actual native frame protocol.
        }
        if (!lost && host.events_ && host.events_->Running()) {
            if (auto progressed = host.events_->Poll(false); progressed.HasError())
                return Output::Failure(progressed.ErrorValue());
            if (host.events_->Running())
                return Output::Success(false);
        }
        if (host.render_) {
            if (auto closed = host.render_->compositor.BeginShutdown(); closed.HasError())
                return Output::Failure(closed.ErrorValue());
            if (lost) {
                if (auto retired = host.render_->RetireViewsAfterLoss(); retired.HasError())
                    return Output::Failure(retired.ErrorValue());
            }
            if (const auto closed = host.render_->color.Close(); closed.HasError() || !closed.Value())
                return closed;
            // Native Close observed actual canonical image retirement and destroyed native views. No submitted
            // signal, CPU handle drop, Scene callback or desktop Present substitutes for this evidence.
            if (auto retired = Editor::EditorVulkanXrViewportAccess::Retire(host.composition_.viewport); retired.HasError())
                return Output::Failure(retired.ErrorValue());
        }
        return Output::Success(true);
    }

    /** @copydoc OpenXRPlayHost::FinishFrameRetirement */
    Result<void> OpenXRPlayHost::FinishFrameRetirement(const XRSessionId &candidate) {
        if (!scene_ || scene_->session != candidate || !scene_->nativeRetired || (render_ && render_->session != candidate))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (render_) {
            if (render_->clockBound) {
                if (auto retired = clock_.RetireNative(candidate); retired.HasError())
                    return retired;
                render_->clockBound = false;
            }
            auto reset = render_->compositor.ResetAfterQuiescence();
            if (reset.HasError())
                return reset;
        }
        // xrDestroySession succeeded only after all imported/native graphics children retired. In Lost recovery
        // that is the explicit quiescence proof permitting abandonment of the old logical frame, never fake End.
        frames_.ResetAfterQuiescence();
        eventsBound_ = false;
        eventsPending_ = false;
        events_.reset();
        render_.reset();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::RetirementComplete */
    Result<bool> OpenXRPlayHost::RetirementComplete(const XRSessionId &session) const {
        if (std::this_thread::get_id() != lane_)
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!session.IsValid())
            return Result<bool>::Failure(MakeError(XRErrors::IdentityInvalid));
        return Result<bool>::Success(retired_ == session && !render_ && !events_ && !eventsBound_ &&
                                     frames_.Snapshot().phase == XRFramePhase::Idle);
    }
}  // namespace Horo::Application::Internal
