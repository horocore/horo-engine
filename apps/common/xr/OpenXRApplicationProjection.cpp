#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRApplicationSession.h"

#include <algorithm>
#include <limits>
#include <string>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRApplicationSession::PrepareProjection */
    Result<OpenXRApplicationProjection> OpenXRApplicationSession::PrepareProjection(const Render::RenderFrontend &frontend,
                                                                                    const Editor::EditorViewportRendererVulkan &viewport,
                                                                                    const OpenXRApplicationBindings &bindings) {
        using Outcome = Result<OpenXRApplicationProjection>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_ || !system_ || !sessionCensus_ || !input_.native || !input_.projection || !input_.bindings ||
            settings_.Profile() != XRFeatureProfile::Projection1_0)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (lastCapabilityRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto observedSystem = native_->PreparedSystem({*preflight_, selection_.attempt});
        if (observedSystem.HasError())
            return Outcome::Failure(observedSystem.ErrorValue());
        const auto selected = Editor::EditorVulkanXrViewportAccess::Device(viewport);
        if (selected.HasError())
            return Outcome::Failure(selected.ErrorValue());
        if (auto graphics = graphics_.ValidatePreparation(); graphics.HasError())
            return Outcome::Failure(graphics.ErrorValue());
        const auto nativeSession = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (nativeSession.HasError())
            return Outcome::Failure(nativeSession.ErrorValue());
        constexpr std::array protocol{"xrWaitFrame",
                                      "xrBeginFrame",
                                      "xrEndFrame",
                                      "xrPollEvent",
                                      "xrBeginSession",
                                      "xrEndSession",
                                      "xrRequestExitSession",
                                      "xrCreateSwapchain",
                                      "xrEnumerateSwapchainImages",
                                      "xrAcquireSwapchainImage",
                                      "xrWaitSwapchainImage",
                                      "xrReleaseSwapchainImage",
                                      "xrDestroySwapchain",
                                      "xrLocateViews"};
        for (const char *name : protocol) {
            PFN_xrVoidFunction function{};
            const auto result = getProc_(nativeSession.Value().instance, name, &function);
            if (result != XR_SUCCESS || !function)
                return Outcome::Failure(MakeError(XRErrors::OperationUnavailable,
                                                  std::string{name} + " unavailable during actual projection preparation (XrResult " +
                                                      std::to_string(result) + ")"));
        }
        const auto stillPrepared = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (stillPrepared.HasError())
            return Outcome::Failure(stillPrepared.ErrorValue());
        if (stillPrepared.Value().incarnation != nativeSession.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        const auto &observed = *system_;
        const auto &session = *sessionCensus_;
        if (observed.primaryViewCount != 2 || observed.configurationCount > observed.configurations.size() ||
            observed.primaryBlendModeCount > observed.primaryBlendModes.size() || session.spaceCount > session.spaces.size() ||
            session.formatCount > session.formats.size())
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto configurations = std::span{observed.configurations}.first(observed.configurationCount);
        const auto blends = std::span{observed.primaryBlendModes}.first(observed.primaryBlendModeCount);
        const auto spaces = std::span{session.spaces}.first(session.spaceCount);
        const auto formats = std::span{session.formats}.first(session.formatCount);
        const auto hasSpace = [&](XrReferenceSpaceType space) {
            return std::ranges::find(spaces, space) != spaces.end();
        };
        const auto hasStereo = std::ranges::find(configurations, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) != configurations.end() &&
                               std::ranges::find(blends, XR_ENVIRONMENT_BLEND_MODE_OPAQUE) != blends.end();
        Render::FramebufferExtent extent;
        for (std::uint32_t view = 0; view < observed.primaryViewCount; ++view) {
            extent.width = std::max(extent.width, observed.primaryViews[view].recommendedImageRectWidth);
            extent.height = std::max(extent.height, observed.primaryViews[view].recommendedImageRectHeight);
        }
        for (std::uint32_t view = 0; view < observed.primaryViewCount; ++view)
            if (!extent.IsValid() || extent.width > observed.primaryViews[view].maxImageRectWidth ||
                extent.height > observed.primaryViews[view].maxImageRectHeight || !observed.primaryViews[view].maxSwapchainSampleCount)
                return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        if (extent.width > observed.system.graphicsProperties.maxSwapchainImageWidth ||
            extent.height > observed.system.graphicsProperties.maxSwapchainImageHeight || !observed.system.graphicsProperties.maxLayerCount)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const Render::RenderTextureDescriptor color{.extent = extent,
                                                    .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                    .layerCount = 2,
                                                    .usage =
                                                        Render::RenderTextureUsage::RenderAttachment | Render::RenderTextureUsage::Sampled};
        const Render::RenderTextureDescriptor depth{.extent = extent,
                                                    .format = Render::RenderTextureFormat::Depth32Float,
                                                    .layerCount = 2,
                                                    .usage = Render::RenderTextureUsage::RenderAttachment};
        const auto &support = frontend.Capabilities().support;
        const auto multiview = Render::ResolveMultiviewCapabilities(support);
        const bool exactImages = support.Supports(color) && support.Supports(depth) && multiview.multipass && multiview.indexedShaders &&
                                 multiview.maximumViews >= 2 &&
                                 std::ranges::find(formats, static_cast<std::int64_t>(VK_FORMAT_R8G8B8A8_UNORM)) != formats.end();
        const auto &budget = settings_.Limits();
        const auto preparedActions = bindings.schema.actions.size() + bindings.poses.size();
        if (preparedActions > budget.maximumActions || budget.maximumSpaces < 5 + bindings.poses.size() ||
            budget.maximumActions > XRActionBindingLimits::MaximumActions || budget.maximumDevices > 3 ||
            budget.maximumSpaces > XRHardLimits::MaximumSpaces)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        auto revision = XRCapabilityRevision::Create(lastCapabilityRevision_ + 1);
        if (revision.HasError())
            return Outcome::Failure(revision.ErrorValue());
        XRCapabilityDescriptor descriptor{.system = observedSystem.Value(),
                                          .revision = revision.Value(),
                                          .limits = {2, budget.maximumSpaces, budget.maximumActions, budget.maximumDevices}};
        descriptor.states.fill(XRCapabilityState::Unsupported);
        const auto fact = [&](XRCapability capability, bool available) {
            descriptor.states[static_cast<std::size_t>(capability)] =
                available ? XRCapabilityState::Available : XRCapabilityState::Unavailable;
        };
        fact(XRCapability::Projection, hasStereo && exactImages);
        fact(XRCapability::PrimaryOpaqueStereo, hasStereo);
        fact(XRCapability::OrientationTracking, observed.system.trackingProperties.orientationTracking == XR_TRUE);
        fact(XRCapability::PositionTracking, observed.system.trackingProperties.positionTracking == XR_TRUE);
        fact(XRCapability::ViewSpace, hasSpace(XR_REFERENCE_SPACE_TYPE_VIEW));
        fact(XRCapability::LocalSpace, hasSpace(XR_REFERENCE_SPACE_TYPE_LOCAL));
        fact(XRCapability::StageSpace, hasSpace(XR_REFERENCE_SPACE_TYPE_STAGE));
        // Action/Input paths were actually prepared; frame/event/swapchain protocol dispatch was resolved above.
        // Native creation and execution still belong to the subsequent resource stages and real event/frame loop.
        for (auto capability :
             {XRCapability::BooleanActions, XRCapability::FloatActions, XRCapability::Vector2Actions, XRCapability::PoseActions,
              XRCapability::CanonicalInputProjection, XRCapability::PredictedFrames, XRCapability::SessionLossLifecycle})
            fact(capability, true);
        fact(XRCapability::ExternalColorTargets, exactImages);
        auto capabilities = XRCapabilitySnapshot::Create(descriptor);
        if (capabilities.HasError())
            return Outcome::Failure(capabilities.ErrorValue());
        auto plan = AdmitXRProjectSettings(settings_, {authorities_.configuration.Snapshot().Revision(), *preflight_, selection_,
                                                       XRRendererCompatibility::Compatible, capabilities.Value(), observedSystem.Value(),
                                                       revision.Value()});
        if (plan.HasError())
            return Outcome::Failure(plan.ErrorValue());
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        ++lastCapabilityRevision_;
        OpenXRApplicationProjection output{std::move(capabilities).Value(), std::move(plan).Value(), color};
        for (std::size_t eye = 0; eye < output.recommended.size(); ++eye) {
            output.recommended[eye] = {observed.primaryViews[eye].recommendedImageRectWidth,
                                       observed.primaryViews[eye].recommendedImageRectHeight};
            output.maximum[eye] = {observed.primaryViews[eye].maxImageRectWidth, observed.primaryViews[eye].maxImageRectHeight};
        }
        return Outcome::Success(std::move(output));
    }
}  // namespace Horo::Application::Internal
