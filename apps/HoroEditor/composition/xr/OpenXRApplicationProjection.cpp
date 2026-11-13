#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRApplicationSession.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    namespace {
        /** @brief Resolves the complete projection/event protocol on the same prepared native instance. */
        template <typename Resolver> Result<void> ValidateProjectionProtocol(const Resolver &getProc_, XrInstance instance) {
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
                const auto result = getProc_(instance, name, &function);
                if (result != XR_SUCCESS || !function)
                    return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable,
                                                           std::format("{} unavailable during actual projection preparation (XrResult {})",
                                                                       name, static_cast<std::int32_t>(result))));
            }
            return Result<void>::Success();
        }

        struct ProjectionTargets final {
            Render::RenderTextureDescriptor color;
            bool stereo{};
            bool exactImages{};
        };

        /** @brief Validates the actual bounded view census and same-device sampled-eye allocation intent. */
        Result<ProjectionTargets> PrepareProjectionTargets(const Render::RenderFrontend &frontend, const NativeSystemCensus &observed,
                                                           const NativeSessionCensus &session) {
            if (observed.primaryViewCount != 2 || observed.configurationCount > observed.configurations.size() ||
                observed.primaryBlendModeCount > observed.primaryBlendModes.size() || session.spaceCount > session.spaces.size() ||
                session.formatCount > session.formats.size())
                return Result<ProjectionTargets>::Failure(MakeError(XRErrors::OperationUnsupported));
            const auto configurations = std::span{observed.configurations}.first(observed.configurationCount);
            const auto blends = std::span{observed.primaryBlendModes}.first(observed.primaryBlendModeCount);
            const auto formats = std::span{session.formats}.first(session.formatCount);
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
                    return Result<ProjectionTargets>::Failure(MakeError(XRErrors::OperationUnsupported));
            if (extent.width > observed.system.graphicsProperties.maxSwapchainImageWidth ||
                extent.height > observed.system.graphicsProperties.maxSwapchainImageHeight ||
                !observed.system.graphicsProperties.maxLayerCount)
                return Result<ProjectionTargets>::Failure(MakeError(XRErrors::OperationUnsupported));
            const Render::RenderTextureDescriptor color{.extent = extent,
                                                        .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                        .layerCount = 2,
                                                        .usage = Render::RenderTextureUsage::RenderAttachment |
                                                                 Render::RenderTextureUsage::Sampled};
            const Render::RenderTextureDescriptor depth{.extent = extent,
                                                        .format = Render::RenderTextureFormat::Depth32Float,
                                                        .layerCount = 2,
                                                        .usage = Render::RenderTextureUsage::RenderAttachment};
            const auto &support = frontend.Capabilities().support;
            const auto multiview = Render::ResolveMultiviewCapabilities(support);
            const bool exactImages = support.Supports(color) && support.Supports(depth) && multiview.multipass &&
                                     multiview.indexedShaders && multiview.maximumViews >= 2 &&
                                     std::ranges::find(formats, static_cast<std::int64_t>(VK_FORMAT_R8G8B8A8_UNORM)) != formats.end();
            return Result<ProjectionTargets>::Success({color, hasStereo, exactImages});
        }

        /** @brief Forms immutable capability evidence from validated native census and exact target support. */
        Result<XRCapabilitySnapshot> ProjectionCapabilities(const NativeSystemCensus &observed, const NativeSessionCensus &session,
                                                            const ProjectionTargets &targets, XRSystemId system,
                                                            XRCapabilityRevision revision, const XRSystemLimits &budget) {
            const auto spaces = std::span{session.spaces}.first(session.spaceCount);
            const auto hasSpace = [&](XrReferenceSpaceType space) {
                return std::ranges::find(spaces, space) != spaces.end();
            };
            XRCapabilityDescriptor descriptor{.system = system,
                                              .revision = revision,
                                              .limits = {2, budget.maximumSpaces, budget.maximumActions, budget.maximumDevices}};
            descriptor.states.fill(XRCapabilityState::Unsupported);
            const auto fact = [&](XRCapability capability, bool available) {
                descriptor.states[static_cast<std::size_t>(capability)] =
                    available ? XRCapabilityState::Available : XRCapabilityState::Unavailable;
            };
            fact(XRCapability::Projection, targets.stereo && targets.exactImages);
            fact(XRCapability::PrimaryOpaqueStereo, targets.stereo);
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
            fact(XRCapability::ExternalColorTargets, targets.exactImages);
            return XRCapabilitySnapshot::Create(descriptor);
        }
    }  // namespace

    /** @copydoc OpenXRApplicationSession::ValidateProjectionPreparation */
    Result<XRSystemId> OpenXRApplicationSession::ValidateProjectionPreparation(const Editor::EditorViewportRendererVulkan &viewport) const {
        using Outcome = Result<XRSystemId>;
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
        if (const auto selected = Editor::EditorVulkanXrViewportAccess::Device(viewport); selected.HasError())
            return Outcome::Failure(selected.ErrorValue());
        if (auto graphics = graphics_.ValidatePreparation(); graphics.HasError())
            return Outcome::Failure(graphics.ErrorValue());
        const auto nativeSession = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (nativeSession.HasError())
            return Outcome::Failure(nativeSession.ErrorValue());
        if (auto protocol = ValidateProjectionProtocol(getProc_, nativeSession.Value().instance); protocol.HasError())
            return Outcome::Failure(protocol.ErrorValue());
        const auto stillPrepared = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (stillPrepared.HasError())
            return Outcome::Failure(stillPrepared.ErrorValue());
        if (stillPrepared.Value().incarnation != nativeSession.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        return Outcome::Success(observedSystem.Value());
    }

    /** @copydoc OpenXRApplicationSession::PrepareProjection */
    Result<OpenXRApplicationProjection> OpenXRApplicationSession::PrepareProjection(const Render::RenderFrontend &frontend,
                                                                                    const Editor::EditorViewportRendererVulkan &viewport,
                                                                                    const OpenXRApplicationBindings &bindings) {
        using Outcome = Result<OpenXRApplicationProjection>;
        const auto observedSystem = ValidateProjectionPreparation(viewport);
        if (observedSystem.HasError())
            return Outcome::Failure(observedSystem.ErrorValue());
        const auto &observed = *system_;
        const auto &session = *sessionCensus_;
        const auto targets = PrepareProjectionTargets(frontend, observed, session);
        if (targets.HasError())
            return Outcome::Failure(targets.ErrorValue());
        const auto &budget = settings_.Limits();
        if (const auto preparedActions = bindings.schema.actions.size() + bindings.poses.size();
            preparedActions > budget.maximumActions || budget.maximumSpaces < 5 + bindings.poses.size() ||
            budget.maximumActions > XRActionBindingLimits::MaximumActions || budget.maximumDevices > 3 ||
            budget.maximumSpaces > XRHardLimits::MaximumSpaces)
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        auto revision = XRCapabilityRevision::Create(lastCapabilityRevision_ + 1);
        if (revision.HasError())
            return Outcome::Failure(revision.ErrorValue());
        auto capabilities = ProjectionCapabilities(observed, session, targets.Value(), observedSystem.Value(), revision.Value(), budget);
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
        OpenXRApplicationProjection output{std::move(capabilities).Value(), std::move(plan).Value(), targets.Value().color};
        for (std::size_t eye = 0; eye < output.recommended.size(); ++eye) {
            output.recommended[eye] = {observed.primaryViews[eye].recommendedImageRectWidth,
                                       observed.primaryViews[eye].recommendedImageRectHeight};
            output.maximum[eye] = {observed.primaryViews[eye].maxImageRectWidth, observed.primaryViews[eye].maxImageRectHeight};
        }
        return Outcome::Success(std::move(output));
    }
}  // namespace Horo::Application::Internal
