#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Foundation/Assertions.h"
#include "Horo/Runtime/Render/MultiviewShaderParameters.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/XR/XRMultiviewScene.h"
#include "OpenXRFrameCompositor.h"
#include "OpenXRPlayFrameState.h"
#include "OpenXRPlayHost.h"
#include "OpenXRVulkanSwapchain.h"

#include <algorithm>
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
                                                           const Render::RenderTextureDescriptor &allocation, std::uint32_t eye,
                                                           const std::uint32_t scale) {
            return {view.id,
                    XRExternalRenderTargetRole::ProjectionColor,
                    acquisition.image.target,
                    acquisition.image,
                    allocation.format,
                    allocation.usage,
                    allocation.extent,
                    NativeProjectionResolution::Rectangle(view.recommendedExtent, scale),
                    eye,
                    2,
                    1};
        }

        /** @brief Assembles the complete frame-scoped stereo plan from the same acquired color image and native ordered views. */
        Result<XRViewRenderPlan> PlanStereoProjection(const XRViewConfigurationId &id, XRViewConfigurationRevision revision,
                                                      XRRenderPredictionTime displayTime, XRWorldOriginRevision origin,
                                                      const NativePresentationViews &observed,
                                                      const OpenXRApplicationProjection &projection,
                                                      const NativeSwapchainAcquisition &acquisition,
                                                      const Render::RenderTextureDescriptor &allocation, const std::uint32_t scale) {
            constexpr std::uint32_t viewCount = 2;
            const XRViewConfigurationDescriptor configuration{CurrentXRContractVersion,
                                                              id.session,
                                                              id,
                                                              revision,
                                                              XRViewConfigurationType::PrimaryStereo,
                                                              XREnvironmentBlendMode::Opaque,
                                                              viewCount};
            std::array<XRViewDescriptor, viewCount> views{};
            std::array<XRExternalRenderTargetDescriptor, viewCount> targets{};
            for (std::uint32_t eye = 0; eye < views.size(); ++eye) {
                views[eye] = DescribeEye(observed, configuration.session, eye, projection);
                targets[eye] = DescribeEyeTarget(views[eye], acquisition, allocation, eye, scale);
            }
            const std::array images{acquisition.image};
            return XRViewRenderPlan::Create({configuration, displayTime, views, targets, images}, configuration.session, configuration.id,
                                            configuration.revision, origin, viewCount, {});
        }

        /** @brief Real device/view dispatch selected by this exact native binding. */
        struct SwapchainGraphics final {
            Render::Detail::VulkanEditorDevice device;
            Render::Detail::VulkanExternalViewDispatch views;
        };

        /** @brief Resolves view ownership on the selected device for both cold and active native allocations. */
        Result<SwapchainGraphics> PrepareSwapchainGraphics(const Editor::EditorViewportRendererVulkan &viewport,
                                                           const XrGraphicsBindingVulkanKHR &binding) {
            using Output = Result<SwapchainGraphics>;
            const auto device = Editor::EditorVulkanXrViewportAccess::Device(viewport);
            if (device.HasError())
                return Output::Failure(device.ErrorValue());
            const auto &selected = device.Value();
            if (!selected.deviceResolver || selected.device != binding.device || selected.instance != binding.instance ||
                selected.physical != binding.physicalDevice || selected.graphicsFamily != binding.queueFamilyIndex)
                return Output::Failure(MakeError(XRErrors::CapabilityStale));
            const Render::Detail::VulkanExternalViewDispatch views{std::bit_cast<PFN_vkCreateImageView>(
                                                                       selected.deviceResolver(selected.device, "vkCreateImageView")),
                                                                   std::bit_cast<PFN_vkDestroyImageView>(
                                                                       selected.deviceResolver(selected.device, "vkDestroyImageView"))};
            if (!views.create || !views.destroy)
                return Output::Failure(MakeError(XRErrors::OperationUnavailable));
            return Output::Success({selected, views});
        }
    }  // namespace

    /** @copydoc OpenXRPlayHost::FrameState::WaitAndImport */
    Result<bool> OpenXRPlayHost::FrameState::WaitAndImport(OpenXRSwapchain &owner, std::optional<NativeSwapchainAcquisition> &image) {
        using enum NativeSwapchainPhase;
        if (owner.Phase() == Available)
            if (auto acquired = owner.Acquire(); acquired.HasError())
                return Result<bool>::Failure(acquired.ErrorValue());
        if (owner.Phase() == Acquired) {
            const auto waited = owner.Wait(0);
            if (waited.HasError() || !waited.Value())
                return waited;
        }
        if (owner.Phase() == Ready) {
            auto imported = owner.Import();
            if (imported.HasError())
                return Result<bool>::Failure(imported.ErrorValue());
            image.emplace(std::move(imported).Value());
        }
        if (owner.Phase() != Imported || !image)
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRPlayHost::FrameState::PrepareEyeViews */
    Result<void> OpenXRPlayHost::FrameState::PrepareEyeViews(Render::RenderFrontend &frontend, const NativeSwapchainAcquisition &image,
                                                             const Render::RenderTextureDescriptor &descriptor,
                                                             const Render::RenderTextureAspect aspect,
                                                             const std::span<Render::RenderTextureViewHandle> views) {
        for (std::uint32_t eye = 0; eye < views.size(); ++eye) {
            auto view = frontend.CreateTextureView(
                {.texture = image.texture, .format = descriptor.format, .aspect = aspect, .baseLayer = eye, .layerCount = 1});
            if (view.HasError())
                return Result<void>::Failure(view.ErrorValue());
            views[eye] = view.Value().handle;
            if (auto processed = frontend.ProcessResourceRequests(); processed.HasError())
                return Result<void>::Failure(processed.ErrorValue());
            if (auto ready = frontend.ResourceOperationResult(view.Value().operation); ready.HasError())
                return ready;
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::ReleaseEyeViews */
    Result<void> OpenXRPlayHost::FrameState::ReleaseEyeViews(Render::RenderFrontend &frontend,
                                                             const std::span<Render::RenderTextureViewHandle> views) {
        for (auto &view : views) {
            if (!view.IsValid())
                continue;
            if (auto released = frontend.ReleaseTextureView(view); released.HasError())
                return released;
            view = {};
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireImage */
    Result<bool> OpenXRPlayHost::FrameState::RetireImage(Render::RenderFrontend &frontend, OpenXRSwapchain &owner,
                                                         const std::span<Render::RenderTextureViewHandle> views) {
        using enum NativeSwapchainPhase;
        if (auto released = ReleaseEyeViews(frontend, views); released.HasError())
            return Result<bool>::Failure(released.ErrorValue());
        if (owner.Phase() == Acquired) {
            const auto ready = owner.Wait(0);
            if (ready.HasError() || !ready.Value())
                return ready;
        }
        return owner.Phase() == Available || owner.Phase() == Empty ? Result<bool>::Success(true) : owner.PollRelease();
    }

    /** @brief Constructs one native auxiliary owner from the selected session's same graphics binding. */
    OpenXRPlayHost::WriterPair::Target::Target(OpenXRPlayHost &host, OpenXRSelectedVulkanBinding &graphics,
                                               const XrGraphicsBindingVulkanKHR &binding, Render::RenderQueueId queue,
                                               Render::Detail::VulkanExternalViewDispatch dispatch)
        : images(host.composition_.frontend, graphics, binding, queue, dispatch),
          owner(host.native_, images, host.composition_.metrics.swapchain) {}

    /** @brief Retains both native targets together until their exact frame and graphics obligations drain. */
    OpenXRPlayHost::WriterPair::WriterPair(OpenXRPlayHost &host, OpenXRSelectedVulkanBinding &graphics,
                                           const XrGraphicsBindingVulkanKHR &binding, const Render::Detail::VulkanEditorDevice &device,
                                           Render::Detail::VulkanExternalViewDispatch views)
        : motion(host, graphics, binding, device.graphicsIdentity, views), depth(host, graphics, binding, device.graphicsIdentity, views) {}

    /** @copydoc OpenXRPlayHost::WriterPair::Stage */
    Result<void> OpenXRPlayHost::WriterPair::Stage(const NativeSessionPreparation &session, const OpenXRProjectionWriter &plan,
                                                   PFN_xrGetInstanceProcAddr getProc) {
        if (auto bound = motion.images.Bind(motion.owner); bound.HasError())
            return bound;
        if (auto bound = depth.images.Bind(depth.owner); bound.HasError())
            return bound;
        const NativePreparedReprojectionSwapchain motionAllocation{{.index = 1, .generation = 1},
                                                                   plan.motion,
                                                                   NativeSwapchainRole::Motion,
                                                                   plan.selection};
        const NativePreparedReprojectionSwapchain depthAllocation{{.index = 2, .generation = 1},
                                                                  plan.depth,
                                                                  NativeSwapchainRole::ReprojectionDepth,
                                                                  plan.selection};
        if (auto staged = motion.owner.StageReprojection(session, motionAllocation, getProc); staged.HasError())
            return staged;
        return depth.owner.StageReprojection(session, depthAllocation, getProc);
    }

    /** @copydoc OpenXRPlayHost::WriterPair::Adopt */
    Result<void> OpenXRPlayHost::WriterPair::Adopt(const XRSessionId &session) {
        if (auto adopted = motion.owner.AdoptReprojection(session); adopted.HasError())
            return adopted;
        auto adopted = depth.owner.AdoptReprojection(session);
        available = adopted.HasValue();
        return adopted;
    }

    /** @copydoc OpenXRPlayHost::WriterPair::Prepare */
    Result<void> OpenXRPlayHost::WriterPair::Prepare(const XRSessionId &session, const OpenXRProjectionWriter &plan,
                                                     PFN_xrGetInstanceProcAddr getProc) {
        const auto prepare = [&](Target &target, const std::uint32_t index, const Render::RenderTextureDescriptor &descriptor,
                                 const NativeSwapchainRole role) -> Result<void> {
            if (auto bound = target.images.Bind(target.owner); bound.HasError())
                return bound;
            return target.owner.Create({.target = {session, {.index = index, .generation = 1}},
                                        .texture = descriptor,
                                        .role = role,
                                        .reprojection = plan.selection},
                                       getProc);
        };
        if (auto ready = prepare(motion, 1, plan.motion, NativeSwapchainRole::Motion); ready.HasError())
            return ready;
        auto ready = prepare(depth, 2, plan.depth, NativeSwapchainRole::ReprojectionDepth);
        available = ready.HasValue();
        return ready;
    }

    /** @copydoc OpenXRPlayHost::WriterPair::Acquire */
    Result<std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages>> OpenXRPlayHost::WriterPair::Acquire(
        Render::RenderFrontend &frontend, const XRFrameId &candidate, const OpenXRProjectionWriter &plan) {
        using Outcome = Result<std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages>>;
        using enum Render::RenderTextureAspect;
        if (frame == candidate && candidate.IsValid())
            return Outcome::Success(ready);
        if (!available || !candidate.IsValid() || frame.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        frame = candidate;
        const auto acquire = [&](Target &entry, const Render::RenderTextureDescriptor &descriptor, Render::RenderTextureAspect aspect) {
            const auto imported = FrameState::WaitAndImport(entry.owner, entry.acquisition);
            if (imported.HasError() || !imported.Value())
                return imported;
            if (auto views = FrameState::PrepareEyeViews(frontend, *entry.acquisition, descriptor, aspect, entry.views); views.HasError())
                return Result<bool>::Failure(views.ErrorValue());
            return Result<bool>::Success(true);
        };
        const auto color = acquire(motion, plan.motion, Color);
        if (color.HasError())
            return Outcome::Failure(color.ErrorValue());
        if (!color.Value())
            return Outcome::Success(std::nullopt);
        const auto depthReady = acquire(depth, plan.depth, Depth);
        if (depthReady.HasError())
            return Outcome::Failure(depthReady.ErrorValue());
        if (!depthReady.Value())
            return Outcome::Success(std::nullopt);
        ready.emplace(motion.acquisition->texture, depth.acquisition->texture, motion.views, depth.views);
        return Outcome::Success(ready);
    }

    /** @copydoc OpenXRPlayHost::WriterPair::AcquireCoordinated */
    Result<std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages>> OpenXRPlayHost::WriterPair::AcquireCoordinated(
        Render::RenderFrontend &frontend, const XRFrameId &candidate, const XRSwapchainImageId &projection) {
        using Outcome = Result<std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages>>;
        using enum Render::RenderTextureAspect;
        if (frame == candidate)
            return Outcome::Success(ready);
        const auto prepared = coordinated->Prepare(candidate, std::span{&projection, 1});
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        if (!prepared.Value())
            return Outcome::Success(std::nullopt);

        nativePair = prepared.Value();
        frame = candidate;
        const auto prepare = [&](const NativeReprojectionAcquisition &source, Target &target, const Render::RenderTextureAspect aspect) {
            return FrameState::PrepareEyeViews(frontend, {source.evidence.image, source.texture}, source.evidence.allocation, aspect,
                                               target.views);
        };
        if (auto views = prepare(nativePair->motion, motion, Color); views.HasError())
            return Outcome::Failure(views.ErrorValue());
        if (auto views = prepare(nativePair->depth, depth, Depth); views.HasError())
            return Outcome::Failure(views.ErrorValue());
        ready.emplace(nativePair->motion.texture, nativePair->depth.texture, motion.views, depth.views);
        return Outcome::Success(ready);
    }

    /** @copydoc OpenXRPlayHost::WriterPair::PollOmitted */
    void OpenXRPlayHost::WriterPair::PollOmitted(Render::RenderFrontend &frontend) {
        if (!coordinated || nativePair)
            return;
        for (auto *target : {&motion, &depth})
            if (FrameState::ReleaseEyeViews(frontend, target->views).HasError())
                return;
        // An unreferenced image remains the coordinator's debt. It cannot be reused until it retires, and Close retries it.
        static_cast<void>(coordinated->PollRetirement());
    }

    /** @copydoc OpenXRPlayHost::WriterPair::Retire */
    Result<bool> OpenXRPlayHost::WriterPair::Retire(Render::RenderFrontend &frontend) {
        if (!frame.IsValid())
            return Result<bool>::Success(true);
        ready.reset();
        if (coordinated) {
            if (nativeSubmitted) {
                for (auto *target : {&motion, &depth})
                    if (auto views = FrameState::ReleaseEyeViews(frontend, target->views); views.HasError())
                        return Result<bool>::Failure(views.ErrorValue());
                return coordinated->PollRetirement();
            }
            if (auto omitted = coordinated->Omit(frame); omitted.HasError())
                return Result<bool>::Failure(omitted.ErrorValue());
            nativePair.reset();
            frame = {};
            PollOmitted(frontend);
            return Result<bool>::Success(true);
        }
        for (auto *target : {&motion, &depth}) {
            const auto retired = FrameState::RetireImage(frontend, target->owner, target->views);
            if (retired.HasError() || !retired.Value())
                return retired;
            target->acquisition.reset();
        }
        frame = {};
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRPlayHost::WriterPair::Close */
    Result<bool> OpenXRPlayHost::WriterPair::Close(Render::RenderFrontend &frontend, const std::optional<XRSessionId> lostSession) {
        using enum NativeSwapchainPhase;
        available = false;
        ready.reset();
        for (auto *target : {&motion, &depth}) {
            if (auto views = FrameState::ReleaseEyeViews(frontend, target->views); views.HasError())
                return Result<bool>::Failure(views.ErrorValue());
            const auto phase = target->owner.Phase();
            const auto closed = lostSession && phase != Empty && phase != Closing && phase != Prepared
                                    ? target->owner.CloseAfterSessionLoss(*lostSession)
                                    : target->owner.Close();
            if (closed.HasError() || !closed.Value())
                return closed;
            target->acquisition.reset();
        }
        frame = {};
        nativePair.reset();
        nativeSubmitted = false;
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRPlayHost::SceneState::StageWriter */
    Result<void> OpenXRPlayHost::SceneState::StageWriter(OpenXRPlayHost &host, OpenXRProjectionDraft &draft) {
        using Viewport = Editor::EditorVulkanXrViewportAccess;
        if (!draft.writer || !prewarmedRenderer || !Viewport::HasPreparedReduction(host.composition_.viewport, *prewarmedRenderer))
            return Result<void>::Success();
        const auto graphics = host.composition_.session.BorrowPreparedGraphics();
        if (graphics.HasError())
            return Result<void>::Failure(graphics.ErrorValue());
        const auto &prepared = graphics.Value();
        const auto selected = PrepareSwapchainGraphics(host.composition_.viewport, prepared.binding);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        preparedWriter =
            std::make_shared<WriterPair>(host, prepared.graphics, prepared.binding, selected.Value().device, selected.Value().views);
        if (auto staged = preparedWriter->Stage(prepared.session, *draft.writer, host.getProc_); staged.HasValue()) {
            auto route =
                host.native_.PrepareReprojectionProvider(prepared.session, preparedWriter->motion.owner, preparedWriter->depth.owner);
            if (route.HasValue()) {
                preparedWriter->preparedProvider.emplace(std::move(route).Value());
                return Result<void>::Success();
            }
        }
        draft.writer.reset();
        const auto closed = preparedWriter->Close(host.composition_.frontend);
        if (closed.HasValue() && closed.Value())
            preparedWriter.reset();
        return Result<void>::Success();
    }

    Result<void> OpenXRPlayHost::SceneState::StageFixedFoveation(OpenXRPlayHost &host, OpenXRProjectionDraft &draft) {
        auto &state = draft.capabilities.states[static_cast<std::size_t>(XRCapability::FixedFoveation)];
        if (state != XRCapabilityState::DependencyMissing)
            return Result<void>::Success();
        const auto graphics = host.composition_.session.BorrowPreparedGraphics();
        if (graphics.HasError())
            return Result<void>::Failure(graphics.ErrorValue());
        const auto &prepared = graphics.Value();
        const auto selected = PrepareSwapchainGraphics(host.composition_.viewport, prepared.binding);
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        const auto &[device, views] = selected.Value();
        preparedColor = std::make_shared<ColorPair>(host, prepared.graphics, prepared.binding, device, views);
        if (auto bound = preparedColor->images.Bind(preparedColor->owner); bound.HasError())
            return bound;
        const auto staged = preparedColor->owner.StageFixedProjection(prepared.session, draft.color, VK_FORMAT_R8G8B8A8_UNORM,
                                                                      draft.recommended, host.getProc_);
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        if (staged.Value())
            state = XRCapabilityState::Available;
        else
            preparedColor.reset();
        return Result<void>::Success();
    }

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
        const auto selected = PrepareSwapchainGraphics(composition_.viewport, *binding.Value());
        if (selected.HasError())
            return Result<void>::Failure(selected.ErrorValue());
        const auto &[device, views] = selected.Value();
        const bool stagedColor = static_cast<bool>(scene_->preparedColor);
        auto storage =
            stagedColor ? scene_->preparedColor : std::make_shared<ColorPair>(*this, *graphics.Value(), *binding.Value(), device, views);
        if (!stagedColor)
            if (auto bound = storage->images.Bind(storage->owner); bound.HasError())
                return bound;
        render_ = std::make_shared<FrameState>(*this, candidate, std::move(storage), color);
        scene_->preparedColor.reset();
        // This is the single actual color allocation for this issued session. The session generation
        // fences its sole target slot; the swapchain owner independently issues every acquisition generation.
        const XRSwapchainTargetId target{candidate, {.index = 0, .generation = 1}};
        const auto created = stagedColor ? render_->color.AdoptFixedProjection(target) : render_->color.Create({target, color}, getProc_);
        if (created.HasError())
            return created;
        if (scene_->projection->writer) {
            if (scene_->preparedWriter) {
                render_->writer = std::exchange(scene_->preparedWriter, {});
                auto &pair = *render_->writer;
                if (auto adopted = pair.Adopt(candidate); adopted.HasError())
                    return adopted;
                auto registered =
                    native_.RegisterPreparedReprojectionProvider(candidate, *pair.preparedProvider, pair.motion.owner, pair.depth.owner);
                if (registered.HasError())
                    return Result<void>::Failure(registered.ErrorValue());
                pair.provider.emplace(std::move(registered).Value());
                pair.preparedProvider.reset();
                pair.coordinated.emplace(frames_, pair.motion.owner, pair.depth.owner);
            } else {
                render_->writer = std::make_shared<WriterPair>(*this, *graphics.Value(), *binding.Value(), device, views);
                const auto prepared = render_->writer->Prepare(candidate, *scene_->projection->writer, getProc_);
                // Optional preparation can leave a native prefix. The same Scene owner keeps it until explicit close.
                if (prepared.HasError())
                    scene_->projection->writer.reset();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FinishFrameResources */
    Result<void> OpenXRPlayHost::FinishFrameResources(const XRSessionId &candidate, const XRCapabilitySnapshot &capabilities) {
        if (!render_ || render_->session != candidate || sessions_->Snapshot().session != candidate || events_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const std::uint32_t layerLimit = scene_ && scene_->passthroughGrant ? 2U : 1U;
        std::optional<XRReprojectionPolicy> reprojection;
        if (render_->writer && render_->writer->provider) {
            const auto &writer = *scene_->projection->writer;
            reprojection.emplace(XRReprojectionPolicy{.motionFormat = writer.motion.format,
                                                      .depthFormat = writer.depth.format,
                                                      .viewCount = static_cast<std::uint32_t>(writer.selection.eyeExtents.size()),
                                                      .motion = XRReprojectionMotionConvention::CurrentNdcMinusPreviousNdcXYZ,
                                                      .cadence = XRReprojectionCadenceMode::ObservedNativePeriods});
            std::ranges::copy(writer.selection.eyeExtents, reprojection->viewExtents.begin());
        }
        if (const auto configured =
                frames_.BindConfiguration({candidate, {.index = 0, .generation = 1}}, capabilities,
                                          {.maximumViews = 2, .maximumImages = reprojection ? 3U : 1U, .maximumLayers = layerLimit},
                                          reprojection);
            configured != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (auto bound = render_->compositor.Bind({getProc_}, {}, NativeCompositorColorSpace::RuntimeDefault, composition_.metrics);
            bound.HasError())
            return bound;
        if (auto bound = clock_.BindNative(); bound.HasError())
            return bound;
        render_->clockBound = true;
        events_.emplace(native_, *sessions_, frames_);
        std::array<OpenXRSwapchain *, 3> owners{&render_->color};
        if (render_->writer) {
            owners[1] = &render_->writer->motion.owner;
            owners[2] = &render_->writer->depth.owner;
        }
        if (auto bound = events_->Bind({getProc_}, std::span{owners}.first(render_->writer ? owners.size() : 1)); bound.HasError())
            return bound;
        eventsBound_ = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::UsesXrScene */
    bool OpenXRPlayHost::UsesXrScene() const noexcept {
        return std::this_thread::get_id() == lane_ && scene_ && scene_->published && !scene_->retiring;
    }

    /** @copydoc OpenXRPlayHost::PresentationState */
    OpenXRPlayPresentation OpenXRPlayHost::PresentationState() const noexcept {
        using enum Horo::Application::Internal::OpenXRPlayPresentation;
        if (std::this_thread::get_id() != lane_ || busy_)
            return Pending;
        if (!scene_ && !render_)
            return Inactive;
        return UsesXrScene() && eventsBound_ && !eventsPending_ && sessions_->Snapshot().admitsFrames &&
                       frames_.Snapshot().phase == XRFramePhase::Idle
                   ? FrameAdmitted
                   : Pending;
    }

    /** @copydoc OpenXRPlayHost::CoverageState */
    OpenXRPlayCoverageStatus OpenXRPlayHost::CoverageState(const XRFrameId &frame) const noexcept {
        if (PresentationState() != OpenXRPlayPresentation::FrameAdmitted || !render_ || render_->coverageFrame != frame)
            return OpenXRPlayCoverageStatus::Unobserved;
        return render_->coverageStatus;
    }

    /** @copydoc OpenXRPlayHost::AbortRenderFrame */
    Result<void> OpenXRPlayHost::AbortRenderFrame() {
        using enum Horo::XR::XRFramePhase;
        if (!render_ || frames_.Snapshot().phase == Idle)
            return Result<void>::Success();
        if (const auto state = frames_.Snapshot(); state.renderAdmitted && state.phase != Aborting &&
                                                   (state.phase != ImagesReleased || render_->rendered) &&
                                                   frames_.Abort(state.frame) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        render_->rendered = false;
        render_->guiFinished = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::CanvasScene */
    Runtime::SceneRuntimeId OpenXRPlayHost::CanvasScene() const noexcept {
        return std::this_thread::get_id() == lane_ && !busy_ && UsesXrScene() ? scene_->scene : Runtime::SceneRuntimeId{};
    }

    /** @copydoc OpenXRPlayHost::PrepareRenderFrame */
    Result<bool> OpenXRPlayHost::PrepareRenderFrame(const std::uint64_t frameNumber, const WorldCanvasFrame *canvas) {
        return PrepareRenderFrame(frameNumber, canvas, 0.0F);
    }

    /** @copydoc OpenXRPlayHost::PrepareRenderFrame(const Runtime::FrameContext &, const WorldCanvasFrame *) */
    Result<bool> OpenXRPlayHost::PrepareRenderFrame(const Runtime::FrameContext &frame, const WorldCanvasFrame *canvas) {
        if (const auto lane = Lane(); lane.HasError())
            return Result<bool>::Failure(lane.ErrorValue());
        return PrepareRenderFrame(frame.frameNumber, canvas, ReadFrameComfort(frame));
    }

    /** @copydoc OpenXRPlayHost::PrepareRenderFrame(std::uint64_t, const WorldCanvasFrame *, float) */
    Result<bool> OpenXRPlayHost::PrepareRenderFrame(const std::uint64_t frameNumber, const WorldCanvasFrame *canvas,
                                                    const float comfortOpacity) {
        using Output = Result<bool>;
        if (auto lane = Lane(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        const Operation operation{*this};
        if (eventsPending_)
            return Output::Success(false);
        if (!UsesXrScene() || !render_ || !eventsBound_)
            return Output::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto valid = scene_->Validate(*this); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (auto tail = ProgressTail(); tail.HasError())
            return Output::Failure(tail.ErrorValue());
        const auto state = frames_.Snapshot();
        if (canvasCompletion_ || (state.phase != XRFramePhase::Idle && (render_->encoded || render_->guiFinished)))
            return Output::Success(false);
        const auto admittedCanvas = PrepareCanvasSource(canvas);
        if (admittedCanvas.HasError())
            return Output::Failure(admittedCanvas.ErrorValue());
        if (state.phase == XRFramePhase::Idle) {
            render_->coverageFrame.reset();
            const auto started = render_->StartPrediction();
            if (started.HasError() || !started.Value())
                return started;
        }
        if (const auto pacing = render_->CheckDeadline(); pacing.HasError() || !pacing.Value())
            return pacing;
        const auto acquired = render_->PrepareProjectionInputs();
        if (acquired.HasError())
            return render_->Fail(acquired.ErrorValue());
        if (!acquired.Value())
            return Output::Success(false);
        if (auto bound = BindRenderFrame(frameNumber, admittedCanvas.Value(), comfortOpacity); bound.HasError())
            return render_->Fail(bound.ErrorValue());
        return Output::Success(true);
    }

    /** @copydoc OpenXRPlayHost::PrepareCanvasSource */
    Result<const OpenXRPlayHost::WorldCanvasFrame *> OpenXRPlayHost::PrepareCanvasSource(const WorldCanvasFrame *canvas) const {
        using Outcome = Result<const WorldCanvasFrame *>;
        if (!canvas)
            return Outcome::Success(nullptr);
        const auto prepared =
            Editor::EditorVulkanXrViewportAccess::PrepareCanvas(composition_.viewport, scene_->geometry->Resources(), canvas->preparation);
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        return Outcome::Success(prepared.Value() ? canvas : nullptr);
    }

    /** @copydoc OpenXRPlayHost::WantsMotionHistory */
    bool OpenXRPlayHost::WantsMotionHistory(const XRFeaturePlan &plan) const noexcept {
        if (scene_ && scene_->geometry && scene_->geometry->mounted)
            return false;
        const auto decision = plan.Decision(XRCapability::SpaceWarp);
        return decision.state == XRFeatureDecisionState::OptionalEnabled ||
               (decision.state == XRFeatureDecisionState::BaselineProjection && decision.observed == XRCapabilityState::DependencyMissing);
    }

    /** @copydoc OpenXRPlayHost::AcquireAuxiliaryProjection */
    std::optional<Editor::EditorVulkanXrViewportAccess::AuxiliaryImages> OpenXRPlayHost::AcquireAuxiliaryProjection(
        const XRFrameId &frame, const XRViewRenderPlan &plan) {
        if (!render_->writer || !scene_->projection->writer || !Editor::EditorVulkanXrViewportAccess::PreviousDepth(composition_.viewport))
            return std::nullopt;
        auto &writer = *render_->writer;
        if (writer.coordinated && (render_->passthrough || !frames_.ObserveReprojectionHistory(frame, plan)))
            return std::nullopt;
        auto pair = writer.coordinated ? writer.AcquireCoordinated(composition_.frontend, frame, render_->acquisition->image)
                                       : writer.Acquire(composition_.frontend, frame, *scene_->projection->writer);
        // Incomplete optional images retain their own cleanup. The baseline binding stays usable.
        return pair.HasValue() ? std::move(pair).Value() : std::nullopt;
    }

    namespace {
        /** @brief Resolves the same eye transforms and minimum raster span for both acquired layers. */
        Result<std::optional<Editor::EditorVulkanXrViewportAccess::CanvasPlacement>> ProjectCanvasPlacement(
            const OpenXRPlayHost::WorldCanvasFrame *canvas, const XRMultiviewScene &scene, const XRMultiviewOwnerState &owner,
            const std::uint32_t minimumSpanPixels) {
            using Viewport = Editor::EditorVulkanXrViewportAccess;
            using Outcome = Result<std::optional<Viewport::CanvasPlacement>>;
            if (!canvas)
                return Outcome::Success(std::nullopt);
            const auto projected = scene.ProjectWorldCanvas({canvas->worldFromCanvas, static_cast<float>(minimumSpanPixels)}, owner);
            if (projected.HasError())
                return Outcome::Failure(projected.ErrorValue());
            if (!projected.Value().drawable)
                return Outcome::Success(std::nullopt);
            const auto &eyes = projected.Value().views;
            return Outcome::Success(Viewport::CanvasPlacement{owner.renderer, {eyes.front().canvasToClip, eyes.back().canvasToClip}});
        }
    }  // namespace

    /** @copydoc OpenXRPlayHost::LightingCutoff */
    std::optional<XRRuntimeTime> OpenXRPlayHost::LightingCutoff(const XRLightEstimateScope &scope) const noexcept {
        const auto read = [&]() -> std::optional<XRRuntimeTime> {
            const auto state = sessions_->Snapshot();
            const auto *tracking = tracking_.Current();
            if (!tracking || pendingRevoke_ || eventsPending_ || !input_.admitted || input_.play != Editor::EditorPlayState::Playing ||
                !state.admitsActions || state.session != scope.session || tracking->Session() != scope.session ||
                tracking->WorldOriginRevision() != scope.space.worldOriginRevision || tracking->Focus() != XRTrackingFocusState::Focused ||
                tracking->State() != XRTrackingState::Active)
                return std::nullopt;
            return tracking->SampledAt();
        };
        const auto observed = read();
        if (!observed || !composition_.session.AllowsNativeLighting(scope) || read() != observed)
            return std::nullopt;
        return observed;
    }

    bool OpenXRPlayHost::FrameLightingAuthorized() const noexcept {
        const auto scope = render_->nativeLightingScope;
        if (!scope)
            return true;
        const auto matches = [&] {
            return scene_ && scene_->nativeLighting && scene_->nativeLighting->scope == *scope && scene_->nativeLighting->active;
        };
        return matches() && LightingCutoff(*scope).has_value() && matches();
    }

    Result<void> OpenXRPlayHost::AbortForLostFrameLighting() {
        const auto scope = render_->nativeLightingScope;
        if (scope && scene_ && scene_->nativeLighting && scene_->nativeLighting->scope == *scope)
            scene_->nativeLighting->active = false;
        return AbortRenderFrame();
    }

    /** @copydoc OpenXRPlayHost::ReadNativeLighting */
    std::optional<XRProjectedLighting> OpenXRPlayHost::ReadNativeLighting(const XRCoordinateSpace &world) {
        if (!scene_->nativeLighting || !scene_->nativeLighting->active)
            return std::nullopt;
        auto &light = *scene_->nativeLighting;
        const auto cutoff = LightingCutoff(light.scope);
        if (!cutoff || light.scope.space != world) {
            light.active = false;
            return std::nullopt;
        }
        const auto space = composition_.session.BorrowWorldSpace(world);
        if (space.HasError() || !LightingCutoff(light.scope)) {
            light.active = false;
            return std::nullopt;
        }
        const auto advanced = native_.AdvanceLightEstimation(light.scope, space.Value(), *cutoff);
        if (!light.active || !LightingCutoff(light.scope)) {
            light.active = false;
            return std::nullopt;
        }

        std::optional<XRLightEstimate> estimate;
        if (advanced.HasValue()) {
            auto current = native_.ReadLightEstimation(light.scope, *cutoff);
            if (!light.active || !LightingCutoff(light.scope)) {
                light.active = false;
                return std::nullopt;
            }
            if (current.HasValue())
                estimate.emplace(std::move(current).Value());
        }
        auto projected = light.projection.Update(light.scope, *cutoff, estimate ? &*estimate : nullptr);
        return projected.HasValue() ? std::move(projected).Value() : std::nullopt;
    }

    /** @copydoc OpenXRPlayHost::ReadFrameLighting */
    std::optional<XRProjectedLighting> OpenXRPlayHost::ReadFrameLighting(const XRFrameSnapshot &frame, const XRCoordinateSpace &world) {
        const auto *source = composition_.lighting;
        if (!Editor::EditorVulkanXrViewportAccess::SupportsEnvironmentLighting(composition_.viewport))
            return std::nullopt;
        if (!source)
            return ReadNativeLighting(world);
        if (!sessions_->Snapshot().admitsActions)
            return std::nullopt;
        const auto sample = source->Read(frame.frame, world, frame.predictedDisplayTime);
        if (!sample || !sample->scope.IsValid() || sample->scope.space != world || sample->scope.session != frame.frame.session ||
            sample->frame != frame.frame || sample->prediction != frame.predictedDisplayTime)
            return std::nullopt;
        return sample->lighting;
    }

    /** @copydoc OpenXRPlayHost::BindRenderFrame */
    Result<void> OpenXRPlayHost::BindRenderFrame(const std::uint64_t frameNumber, const WorldCanvasFrame *canvas,
                                                 const float comfortOpacity) {
        if (!scene_ || !scene_->projection || !render_->views || !render_->acquisition)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto frame = frames_.Snapshot();
        const auto &support = composition_.frontend.Capabilities().support;
        const auto revision = XRViewConfigurationRevision::Create(scene_->projection->capabilities.Revision().Value());
        if (revision.HasError())
            return Result<void>::Failure(revision.ErrorValue());
        const XRViewConfigurationId configuration{render_->session, {.index = 0, .generation = 1}};
        const auto fixed = scene_->projection->plan.Decision(XRCapability::FixedFoveation).state;
        const bool adaptive = !support.synthetic && fixed != XRFeatureDecisionState::OptionalEnabled &&
                              fixed != XRFeatureDecisionState::Required && !WantsMotionHistory(scene_->projection->plan);
        const auto scale = render_->resolution.BeginFrame(frame.frame, adaptive, support.deviceIncarnation, support.capabilityRevision);
        auto plan = PlanStereoProjection(configuration, revision.Value(), frame.predictedDisplayTime, input_.origin, *render_->views,
                                         *scene_->projection, *render_->acquisition, render_->allocation, scale);
        if (plan.HasError())
            return Result<void>::Failure(plan.ErrorValue());
        const Render::MultiviewPublication publication{scene_->scene.value,       frameNumber,
                                                       input_.origin.Value(),     revision.Value().Value(),
                                                       support.deviceIncarnation, support.capabilityRevision};
        const auto nativeReference = render_->ReferencePose(*scene_, frame, input_.origin);
        if (nativeReference.HasError())
            return Result<void>::Failure(nativeReference.ErrorValue());
        const auto reference = render_->GameplayReferencePose(*scene_, nativeReference.Value());
        if (reference.HasError())
            return Result<void>::Failure(reference.ErrorValue());
        auto converted =
            render_->ConvertScene(plan.Value(), configuration, revision.Value(), input_.origin, reference.Value(), publication);
        if (converted.HasError())
            return Result<void>::Failure(converted.ErrorValue());
        const Editor::EditorVulkanXrViewportAccess::MotionInput motion{{publication, frame.frame.sequence,
                                                                        frame.predictedDisplayTime.Nanoseconds()},
                                                                       scene_->geometry->MotionObjects()};
        render_->passthrough.reset();
        if (const auto scope = passthroughScope_;
            scope && HasPassthrough(*scope) && passthroughScope_ == scope && nativeReference.Value().Target() == scope->world)
            render_->passthrough.emplace(*scope, publication, nativeReference.Value());
        const auto alpha = render_->passthrough ? Editor::EditorXrProjectionAlpha::Premultiplied : Editor::EditorXrProjectionAlpha::Opaque;
        using Viewport = Editor::EditorVulkanXrViewportAccess;
        const std::array images{render_->acquisition->image};
        const XRMultiviewOwnerState owner{render_->session,
                                          configuration,
                                          revision.Value(),
                                          input_.origin,
                                          images,
                                          publication,
                                          Render::ResolveMultiviewCapabilities(support)};
        const auto projected = ProjectCanvasPlacement(canvas, converted.Value(), owner, scene_->projection->worldCanvasMinimumSpanPixels);
        if (projected.HasError())
            return Result<void>::Failure(projected.ErrorValue());
        const auto &placement = projected.Value();
        const auto lighting = ReadFrameLighting(frame, reference.Value().Target());
        const Render::MultiviewEnvironmentLighting environment =
            lighting ? Render::MultiviewEnvironmentLighting{lighting->ambient, lighting->directional}
                     : Render::MultiviewEnvironmentLighting{};
        const auto *environmentInput = lighting && environment.IsValid() ? &environment : nullptr;
        const float vignette = alpha == Editor::EditorXrProjectionAlpha::Opaque ? comfortOpacity : 0.0F;
        // A canvas or display-locked comfort effect has no signed per-pixel motion writer.
        const bool virtualPlacement = scene_->locomotion && scene_->locomotion->worldPlacement.has_value();
        const bool ordinaryOnly = placement.has_value() || vignette > 0.0F || virtualPlacement;
        const bool prepareMotion = !ordinaryOnly && WantsMotionHistory(scene_->projection->plan);
        if (!ordinaryOnly && render_->writer && render_->writer->coordinated)
            static_cast<void>(frames_.RecordReprojectionReference(frame.frame, reference.Value()));
        std::array<Render::MultiviewRenderRectangle, 2> rectangles;
        std::ranges::transform(plan.Value().Targets(), rectangles.begin(), [](const XRExternalRenderTargetDescriptor &target) {
            const auto &region = target.renderRectangle;
            return Render::MultiviewRenderRectangle{region.x, region.y, region.width, region.height};
        });
        const auto bind = [&](const Viewport::AuxiliaryImages *auxiliary) {
            return Viewport::Bind(composition_.viewport, render_->acquisition->texture, render_->eyeViews, publication,
                                  converted.Value().Scene().Views(), scene_->geometry->Instances(), prepareMotion ? &motion : nullptr,
                                  alpha, auxiliary, environmentInput, &rectangles, placement ? &*placement : nullptr, vignette);
        };
        if (auto baseline = bind(nullptr); baseline.HasError())
            return baseline;
        if (const auto pair = ordinaryOnly ? std::nullopt : AcquireAuxiliaryProjection(frame.frame, plan.Value());
            pair && bind(&*pair).HasError())
            if (auto baseline = bind(nullptr); baseline.HasError())
                return baseline;
        if (prepareMotion)
            render_->motionFrame = motion.frame;
        render_->plan.emplace(std::move(plan).Value());
        if (environmentInput && !composition_.lighting && scene_->nativeLighting)
            render_->nativeLightingScope = scene_->nativeLighting->scope;
        render_->hostFrameNumber = frameNumber;
        render_->canvasBound = placement.has_value();
        render_->vignetteBound = vignette > 0.0F;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::ExecuteRenderFrame */
    Result<void> OpenXRPlayHost::ExecuteRenderFrame(Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output,
                                                    const Editor::EditorVulkanXrViewportAccess::CanvasSource *canvas) {
        if (auto lane = Lane(); lane.HasError())
            return lane;
        if (!UsesXrScene() || !render_ || !render_->plan || !render_->acquisition || render_->encoded)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{*this};
        if (!FrameLightingAuthorized())
            return AbortForLostFrameLighting();
        auto executed =
            Editor::EditorVulkanXrViewportAccess::Execute(composition_.viewport, frame, output, render_->canvasBound ? canvas : nullptr);
        if (executed.HasError()) {
            const auto aborted = AbortRenderFrame();
            (void)aborted;
            return Result<void>::Failure(executed.ErrorValue());
        }
        render_->encoded = std::move(executed).Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::TakeCanvasCompletion */
    std::optional<OpenXRPlayHost::WorldCanvasCompletion> OpenXRPlayHost::TakeCanvasCompletion() noexcept {
        if (std::this_thread::get_id() != lane_ || busy_)
            return std::nullopt;
        return std::exchange(canvasCompletion_, std::nullopt);
    }

    /** @copydoc OpenXRPlayHost::ReadTrackedReference */
    Result<OpenXRTrackedReference> OpenXRPlayHost::ReadTrackedReference(const XRSessionId session,
                                                                        const std::uint64_t hostFrameNumber) const {
        using Output = Result<OpenXRTrackedReference>;
        const auto references = ReadReferences(session);
        if (references.HasError())
            return Output::Failure(references.ErrorValue());
        const auto frame = frames_.Snapshot();
        if (!UsesXrScene() || !sessions_->Snapshot().admitsFrames || !render_ || !render_->views || !render_->plan ||
            render_->hostFrameNumber != hostFrameNumber || !frame.renderAdmitted || frame.frame.session != session ||
            !frame.frame.IsValid() || (frame.phase != XRFramePhase::ImagesAcquired && frame.phase != XRFramePhase::RendererSubmitted))
            return Output::Failure(MakeError(XRErrors::IdentityStale));
        const auto nativeReference = render_->ReferencePose(*scene_, frame, input_.origin);
        if (nativeReference.HasError())
            return Output::Failure(nativeReference.ErrorValue());
        auto pose = render_->GameplayReferencePose(*scene_, nativeReference.Value());
        if (pose.HasError())
            return Output::Failure(pose.ErrorValue());
        const auto &components = pose.Value().Components();
        if (components.positionMeters.validity != XRPoseComponentValidity::Tracked ||
            components.orientation.validity != XRPoseComponentValidity::Tracked || components.loss != XRTrackingLossState::None)
            return Output::Failure(MakeError(XRErrors::OperationUnavailable));
        return Output::Success({frame.frame, std::move(pose).Value()});
    }

    /** @copydoc OpenXRPlayHost::RevalidateRenderLighting */
    Result<bool> OpenXRPlayHost::RevalidateRenderLighting(const std::uint64_t frameNumber) {
        using Allowed = Result<bool>;
        if (auto lane = Lane(); lane.HasError())
            return Allowed::Failure(lane.ErrorValue());
        if (!render_ || render_->hostFrameNumber != frameNumber || frames_.Snapshot().phase == XRFramePhase::Idle)
            return Allowed::Failure(MakeError(XRErrors::IdentityStale));
        const Operation operation{*this};
        if (FrameLightingAuthorized())
            return Allowed::Success(true);
        auto aborted = AbortForLostFrameLighting();
        return aborted.HasError() ? Allowed::Failure(aborted.ErrorValue()) : Allowed::Success(false);
    }

    /** @copydoc OpenXRPlayHost::MirrorView */
    Editor::EditorViewportTextureView OpenXRPlayHost::MirrorView(const std::uint64_t frameNumber) const noexcept {
        if (std::this_thread::get_id() != lane_ || busy_ || !UsesXrScene() || !render_ || !render_->encoded || render_->guiFinished ||
            !render_->plan || !render_->acquisition || render_->hostFrameNumber != frameNumber ||
            sessions_->Snapshot().state == XRSessionState::Lost)
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
        if (!render_ || render_->guiFinished || frames_.Snapshot().phase == XRFramePhase::Idle || (accepted && !render_->encoded))
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{*this};
        if (accepted && !FrameLightingAuthorized())
            return AbortForLostFrameLighting();
        render_->guiFinished = true;
        render_->rendered = accepted && render_->encoded && UsesXrScene();
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
        if (auto retired = composition_.session.ProgressSpatialAnchorRetirement(); retired.HasError())
            return retired;
        if (auto retired = composition_.session.ProgressPlaneRetirement(); retired.HasError())
            return retired;
        if (render_ && render_->writer && !eventsPending_ && sessions_->Snapshot().state != XRSessionState::Lost)
            render_->writer->PollOmitted(composition_.frontend);
        if (eventsPending_ || !render_ || frames_.Snapshot().phase == XRFramePhase::Idle)
            return Result<void>::Success();
        if (sessions_->Snapshot().state == XRSessionState::Lost)
            return Result<void>::Success();  // Explicit destructive retirement owns this debt; ordinary End is no longer admitted.
        if (!render_->guiFinished && !(scene_ && scene_->retiring))
            return Result<void>::Success();
        if (scene_ && scene_->retiring) {
            auto aborted = AbortRenderFrame();
            if (aborted.HasError())
                return aborted;
        } else if (render_->rendered && !FrameLightingAuthorized()) {
            if (auto aborted = AbortForLostFrameLighting(); aborted.HasError())
                return aborted;
        }
        if (const auto state = frames_.Snapshot();
            render_->encoded && state.phase != XRFramePhase::RendererSubmitted && state.phase != XRFramePhase::ImagesReleased) {
            const auto admitted = render_->AdmitRecordedProjection(state.frame);
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            if (!admitted.Value())
                return Result<void>::Success();
        }
        const auto state = frames_.Snapshot();
        if (render_->encoded) {
            const auto polled = Editor::EditorVulkanXrViewportAccess::Poll(composition_.viewport);
            if (polled.HasError())
                return polled;
            render_->ObserveRenderCompletion(state.frame);
            render_->ObserveCoverage(state.frame);
        }
        const auto retired = render_->RetireImageReferences();
        if (retired.HasError())
            return Result<void>::Failure(retired.ErrorValue());
        if (!retired.Value())
            return Result<void>::Success();
        if (render_->acquisitionRecorded && render_->acquisition && state.phase != XRFramePhase::ImagesReleased &&
            frames_.Release(state.frame, render_->acquisition->image) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const bool hasProjection = render_->rendered && render_->plan && render_->acquisition;
        const auto ended = hasProjection ? render_->EndRenderedProjection(state.frame) : render_->compositor.EndEmpty(state.frame);
        if (ended.HasError())
            return ended;
        const bool projected = hasProjection && render_->rendered;
        if (projected)
            composition_.metrics.RecordAcceptedProjectionComfort(render_->vignetteBound);
        render_->CompleteMotion(projected);
        if (render_->writer && render_->writer->nativeSubmitted) {
            render_->writer->nativePair.reset();
            render_->writer->nativeSubmitted = false;
            render_->writer->frame = {};
        }
        if (std::exchange(render_->canvasBound, false))
            canvasCompletion_ = WorldCanvasCompletion{render_->hostFrameNumber, projected};
        render_->encoded.reset();
        render_->acquisition.reset();
        render_->plan.reset();
        render_->passthrough.reset();
        render_->nativeLightingScope.reset();
        render_->views.reset();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::AdmitRecordedProjection */
    Result<bool> OpenXRPlayHost::FrameState::AdmitRecordedProjection(const XRFrameId &frame) {
        using Viewport = Editor::EditorVulkanXrViewportAccess;
        using Outcome = Result<bool>;
        const auto outcome = Viewport::ResolveSubmission(host.composition_.viewport, *encoded);
        if (outcome.HasError())
            return Outcome::Failure(outcome.ErrorValue());
        if (outcome.Value() == Viewport::SubmissionState::Pending)
            return Outcome::Success(false);
        if (outcome.Value() == Viewport::SubmissionState::Rejected) {
            encoded.reset();
            if (auto aborted = host.AbortRenderFrame(); aborted.HasError())
                return Outcome::Failure(aborted.ErrorValue());
            return Outcome::Success(true);
        }
        if (!rendered)
            return Outcome::Success(true);  // Accepted GPU work still owes retirement for this aborted frame.
        const auto &support = host.composition_.frontend.Capabilities().support;
        if (!acquisition || encoded->color != acquisition->texture || encoded->publication.frame != hostFrameNumber ||
            encoded->publication.deviceIncarnation != support.deviceIncarnation ||
            encoded->publication.capabilityRevision != support.capabilityRevision) {
            const auto aborted = host.AbortRenderFrame();
            return aborted.HasError() ? Outcome::Failure(aborted.ErrorValue()) : Outcome::Success(true);
        }
        const std::array images{acquisition->image};
        if (writer && writer->nativePair) {
            if (auto polled = Viewport::Poll(host.composition_.viewport); polled.HasError())
                return Outcome::Failure(polled.ErrorValue());
            if (!Viewport::SubmissionCompleted(host.composition_.viewport, *encoded))
                return Outcome::Success(false);
            ObserveCoverage(frame);
            if (SubmitSynthesis(frame, images))
                return Outcome::Success(true);
        }
        if (host.frames_.Submit(frame, images) != XRFrameStatus::Ok)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        return Outcome::Success(true);
    }

    /** @copydoc OpenXRPlayHost::FrameState::ObserveRenderCompletion */
    void OpenXRPlayHost::FrameState::ObserveRenderCompletion(const XRFrameId &frame) {
        const auto &metrics = host.composition_.metrics;
        if (completionTimingObserved || !rendered || !encoded ||
            (!metrics.MeasuresRenderCompletion() && !resolution.AwaitsCompletion(frame)) ||
            !Editor::EditorVulkanXrViewportAccess::SubmissionCompleted(host.composition_.viewport, *encoded))
            return;
        completionTimingObserved = true;
        const auto pacing = ObservePacing(frame);
        if (pacing.HasValue())
            resolution.ObserveCompletion(pacing.Value());
        metrics.RecordRenderCompletion(pacing.HasValue() ? std::optional{pacing.Value().deadline} : std::nullopt);
    }

    /** @copydoc OpenXRPlayHost::FrameState::ObserveCoverage */
    void OpenXRPlayHost::FrameState::ObserveCoverage(const XRFrameId &frame) noexcept {
        if (coverageFrame || !rendered || !encoded || !motionFrame || !writer || !writer->ready || writer->frame != frame)
            return;
        const auto reduction = Editor::EditorVulkanXrViewportAccess::TakeCoverageReduction(host.composition_.viewport, *motionFrame);
        if (!reduction || reduction->completion != encoded->signal)
            return;
        const auto &actual = reduction->auxiliary;
        const auto &expected = *writer->ready;
        if (actual.motion != expected.motion || actual.depth != expected.depth || actual.motionViews != expected.motionViews ||
            actual.depthViews != expected.depthViews)
            return;
        coverageFrame = frame;
        const bool complete = reduction->Complete();
        coverageStatus = complete ? OpenXRPlayCoverageStatus::Complete : OpenXRPlayCoverageStatus::Incomplete;
        if (complete)
            this->reduction = reduction;
    }

    /** @copydoc OpenXRPlayHost::FrameState::SubmitSynthesis */
    bool OpenXRPlayHost::FrameState::SubmitSynthesis(const XRFrameId &frame, const std::span<const XRSwapchainImageId> projection) {
        if (!reduction || !plan || !writer->provider || host.native_.BorrowReprojectionProvider(*writer->provider).HasError())
            return false;
        const auto history = host.frames_.ObserveReprojectionHistory(frame, *plan);
        if (!history || reduction->predecessor.projectionSequence != history->previousFrameSequence ||
            reduction->predecessor.predictedDisplayTimeNanoseconds != history->previousPrediction.Nanoseconds())
            return false;
        const auto &extents = writer->nativePair->motion.eyeExtents;
        for (std::size_t eye = 0; eye < extents.size(); ++eye)
            if (reduction->rectangles[eye] != Render::MultiviewRenderRectangle{0, 0, extents[eye].width, extents[eye].height})
                return false;
        if (writer->coordinated->Submit(frame, projection).HasError())
            return false;
        synthesisHistory = history;
        writer->nativeSubmitted = true;
        return true;
    }

    /** @copydoc OpenXRPlayHost::FrameState::EndRenderedProjection */
    Result<void> OpenXRPlayHost::FrameState::EndRenderedProjection(const XRFrameId &frame) {
        const std::array targets{NativeProjectionTarget{&color, acquisition->image}};
        if (passthrough && host.passthroughScope_ == passthrough->scope && host.scene_ && host.scene_->passthroughGrant &&
            !host.composition_.session.AllowsPassthrough(passthrough->scope)) {
            host.RevokePassthrough();
            host.passthroughState_ = XRCapabilityState::Denied;
        }
        const bool allowPassthrough = passthrough && host.HasPassthrough(passthrough->scope);
        if (!host.FrameLightingAuthorized()) {
            if (auto aborted = host.AbortForLostFrameLighting(); aborted.HasError())
                return aborted;
            return compositor.EndEmpty(frame);
        }
        if (allowPassthrough && passthrough && host.MatchesPassthroughGrant(passthrough->scope)) {
            const auto receipt =
                Editor::EditorVulkanXrViewportAccess::TakePremultipliedProjection(host.composition_.viewport, passthrough->publication,
                                                                                  acquisition->texture);
            if (receipt)
                return compositor.EndPassthroughProjection(frame, *plan, targets,
                                                           {frame, passthrough->scope, passthrough->referenceToWorld});
        }
        if (writer && writer->nativeSubmitted)
            return EndSynthesis(frame, targets);
        return compositor.EndProjection(frame, *plan, targets);
    }

    /** @copydoc OpenXRPlayHost::FrameState::EndSynthesis */
    Result<void> OpenXRPlayHost::FrameState::EndSynthesis(const XRFrameId &frame,
                                                          const std::span<const NativeProjectionTarget> projection) {
        const auto &history = *synthesisHistory;
        const auto &acquired = *writer->nativePair;
        std::array<XRReprojectionViewInput, 2> inputs{};
        std::array<XRReprojectionViewCoverage, 2> coverage{};
        for (std::size_t eye = 0; eye < inputs.size(); ++eye) {
            const auto [x, y, width, height] = reduction->rectangles[eye];
            const XRRenderRectangle rectangle{x, y, width, height};
            const auto layer = static_cast<std::uint32_t>(eye);
            auto &input = inputs[eye];
            input = {plan->Views()[eye].id,
                     {acquired.motion.evidence.image, rectangle, layer},
                     {acquired.depth.evidence.image, rectangle, layer},
                     ProjectionDepth.minimumDepth,
                     ProjectionDepth.maximumDepth,
                     ProjectionDepth.nearMeters,
                     ProjectionDepth.farMeters};
            const auto &tally = reduction->tallies[eye];
            coverage[eye] = {input.view, input.motion, input.depth, tally.visited, tally.unresolved};
        }
        const XRReprojectionInputs evidence{history.previousPrediction, history.previousFrameSequence, history.applicationDelta, inputs,
                                            XRReprojectionCoverage{frame.session, frame.sequence, plan->PredictedDisplayTime(),
                                                                   history.previousFrameSequence, history.previousPrediction, coverage}};
        const auto ended = compositor.EndReprojectedProjection(frame, *plan, projection,
                                                               {*writer->provider, writer->motion.owner, writer->depth.owner, evidence});
        return ended.HasError() ? Result<void>::Failure(ended.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc OpenXRPlayHost::FrameState::CompleteMotion */
    void OpenXRPlayHost::FrameState::CompleteMotion(const bool projected) noexcept {
        Editor::EditorVulkanXrViewportAccess::CompleteMotion(host.composition_.viewport, motionFrame, projected);
        if (!projected)
            coverageFrame.reset();
        motionFrame.reset();
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireViewsAfterLoss */
    Result<bool> OpenXRPlayHost::FrameState::RetireViewsAfterLoss() {
        using Viewport = Editor::EditorVulkanXrViewportAccess;
        using Outcome = Result<bool>;
        // Loss closes canonical admission, not GPU work. Preserve the same queue references and retry debt.
        if (encoded) {
            const auto resolved = Viewport::ResolveSubmission(host.composition_.viewport, *encoded);
            if (resolved.HasError())
                return Outcome::Failure(resolved.ErrorValue());
            if (resolved.Value() == Viewport::SubmissionState::Pending)
                return Outcome::Success(false);
            if (resolved.Value() == Viewport::SubmissionState::Rejected)
                encoded.reset();
            else if (auto polled = Viewport::Poll(host.composition_.viewport); polled.HasError())
                return Outcome::Failure(polled.ErrorValue());
        }
        const auto released = ReleaseEyeViews(host.composition_.frontend, eyeViews);
        return released.HasError() ? Outcome::Failure(released.ErrorValue()) : Outcome::Success(true);
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireResources */
    Result<bool> OpenXRPlayHost::FrameState::RetireResources(OpenXRPlayHost &host, const XRSessionId &candidate) {
        using Output = Result<bool>;
        if (host.render_ && host.render_->session != candidate)
            return Output::Failure(MakeError(XRErrors::IdentityStale));
        if (host.sessions_->Snapshot().state != XRSessionState::Lost && host.events_ && (host.eventsPending_ || host.events_->Running())) {
            const auto observed = host.events_->Poll(false);
            if (observed.HasError()) {
                host.RevokeEventAdmission();
                return Output::Failure(observed.ErrorValue());
            }
            if (const auto applied = host.ApplyEventObservation(observed.Value()); applied.HasError())
                return Output::Failure(applied.ErrorValue());
        }
        const bool lost = host.sessions_->Snapshot().state == XRSessionState::Lost;
        if (!lost && host.eventsPending_)
            return Output::Success(false);
        if (!lost) {
            if (auto tail = host.ProgressTail(); tail.HasError())
                return Output::Failure(tail.ErrorValue());
            if (host.frames_.Snapshot().phase != XRFramePhase::Idle)
                return Output::Success(false);  // Normal stopping must finish the actual native frame protocol.
        }
        if (!lost && host.events_ && host.events_->Running())
            return Output::Success(false);
        return RetireGraphics(host, lost);
    }

    /** @copydoc OpenXRPlayHost::FrameState::RetireGraphics */
    Result<bool> OpenXRPlayHost::FrameState::RetireGraphics(OpenXRPlayHost &host, const bool lost) {
        using Output = Result<bool>;
        if (host.render_) {
            if (auto closed = host.render_->compositor.BeginShutdown(); closed.HasError())
                return Output::Failure(closed.ErrorValue());
            if (lost) {
                if (auto retired = host.render_->RetireViewsAfterLoss(); retired.HasError() || !retired.Value())
                    return retired;
            }
            if (host.render_->writer) {
                if (auto &provider = host.render_->writer->provider) {
                    host.native_.RevokeReprojectionProvider(*provider);
                    provider.reset();
                }
                const auto closed =
                    host.render_->writer->Close(host.composition_.frontend, lost ? std::optional{host.render_->session} : std::nullopt);
                if (closed.HasError() || !closed.Value())
                    return closed;
            }
            if (const auto closed = lost ? host.render_->color.CloseAfterSessionLoss(host.render_->session) : host.render_->color.Close();
                closed.HasError() || !closed.Value())
                return closed;
            // Native Close observed actual canonical image retirement and destroyed native views. No submitted
            // signal, CPU handle drop, Scene callback or desktop Present substitutes for this evidence.
        }
        // Cold renderer preparation can precede the canonical session ID and its native frame owner.
        if (auto retired = Editor::EditorVulkanXrViewportAccess::Retire(host.composition_.viewport); retired.HasError())
            return Output::Failure(retired.ErrorValue());
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
        if (render_ && render_->canvasBound)
            canvasCompletion_ = WorldCanvasCompletion{render_->hostFrameNumber, false};
        render_.reset();
        passthroughScope_.reset();
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
