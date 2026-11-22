#include "EditorVulkanXrViewportAccess.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRApplicationSession.h"
#include "OpenXRVulkanSwapchain.h"

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

        /** @brief Common stereo target and required format support observed on the native runtime. */
        struct NativeProjectionTargets final {
            Render::RenderTextureDescriptor color;
            bool stereo{};
            bool colorAvailable{};
        };

        /** @brief Resolves the common native stereo target used by both the packaged probe and live Play. */
        Result<NativeProjectionTargets> ObserveNativeProjectionTargets(const NativeSystemCensus &observed,
                                                                       const NativeSessionCensus &session) {
            using Outcome = Result<NativeProjectionTargets>;
            if (observed.configurationCount > observed.configurations.size() ||
                observed.primaryBlendModeCount > observed.primaryBlendModes.size() || session.spaceCount > session.spaces.size() ||
                session.formatCount > session.formats.size())
                return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
            if (observed.primaryViewCount != 2)
                return Outcome::Failure(
                    MakeError(XRErrors::OperationUnsupported, "The selected projection requires exactly two stereo views."));
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
                    return Outcome::Failure(
                        MakeError(XRErrors::OperationUnsupported, "Stereo view or system limits reject the shared swapchain extent."));
            if (extent.width > observed.system.graphicsProperties.maxSwapchainImageWidth ||
                extent.height > observed.system.graphicsProperties.maxSwapchainImageHeight ||
                !observed.system.graphicsProperties.maxLayerCount)
                return Outcome::Failure(
                    MakeError(XRErrors::OperationUnsupported, "Stereo view or system limits reject the shared swapchain extent."));
            const Render::RenderTextureDescriptor color{.extent = extent,
                                                        .format = Render::RenderTextureFormat::Rgba8Unorm,
                                                        .layerCount = 2,
                                                        .usage = Render::RenderTextureUsage::RenderAttachment |
                                                                 Render::RenderTextureUsage::Sampled};
            const auto colorAvailable = std::ranges::find(formats, static_cast<std::int64_t>(VK_FORMAT_R8G8B8A8_UNORM)) != formats.end();
            return Outcome::Success({color, hasStereo, colorAvailable});
        }

        /** @brief Checks the selected render device's baseline raster requirements against the same native target. */
        Result<ProjectionTargets> PrepareProjectionTargets(const Render::RenderFrontend &frontend, const NativeSystemCensus &observed,
                                                           const NativeSessionCensus &session) {
            const auto native = ObserveNativeProjectionTargets(observed, session);
            if (native.HasError())
                return Result<ProjectionTargets>::Failure(native.ErrorValue());
            const auto &[color, stereo, colorAvailable] = native.Value();
            const Render::RenderTextureDescriptor depth{.extent = color.extent,
                                                        .format = Render::RenderTextureFormat::Depth32Float,
                                                        .layerCount = 2,
                                                        .usage = Render::RenderTextureUsage::RenderAttachment};
            const auto &support = frontend.Capabilities().support;
            const auto multiview = Render::ResolveMultiviewCapabilities(support);
            const bool exactImages = colorAvailable && support.Supports(color) && support.Supports(depth) && multiview.multipass &&
                                     multiview.indexedShaders && multiview.maximumViews >= 2;
            return Result<ProjectionTargets>::Success({color, stereo, exactImages});
        }

        struct ReprojectionEvidence final {
            XRCapabilityState observed{XRCapabilityState::Unsupported};
            std::optional<OpenXRProjectionWriter> writer;
        };

        /** @brief Admits exact selected-device writer allocations after validating the cooked motion pipeline. */
        std::optional<OpenXRProjectionWriter> PrepareOptionalWriter(const NativeReprojectionOffer &offer,
                                                                    const XRLoaderPreflightAttempt attempt,
                                                                    const Render::RenderFrontend &frontend,
                                                                    Editor::EditorViewportRendererVulkan &viewport,
                                                                    const NativeSessionCensus &session) {
            using namespace Render;
            const auto &[left, right] = offer.extents;
            if (offer.availabilityWithoutProducer != XRCapabilityState::DependencyMissing || !left.IsValid() || !right.IsValid())
                return std::nullopt;
            const FramebufferExtent extent{std::max(left.width, right.width), std::max(left.height, right.height)};
            const auto formats = SelectOpenXRVulkanReprojectionFormats(frontend.Capabilities().support,
                                                                       std::span{session.formats}.first(session.formatCount), extent);
            if (!formats)
                return std::nullopt;
            const auto pipeline = Editor::EditorVulkanXrViewportAccess::PrepareMotionPipeline(viewport, formats->motion, formats->depth);
            if (pipeline.HasError() || !pipeline.Value())
                return std::nullopt;
            constexpr auto usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled;
            const RenderTextureDescriptor motion{.extent = extent, .format = formats->motion, .layerCount = 2, .usage = usage};
            auto depth = motion;
            depth.format = formats->depth;
            return OpenXRProjectionWriter{{attempt, offer.route, offer.extents}, motion, depth};
        }

        /** @brief Observes compositor availability independently of the selected writer's preparation. */
        Result<ReprojectionEvidence> PrepareOptionalReprojection(const OpenXRNativeSession &native, XRLoaderPreflightAttempt attempt,
                                                                 std::span<const XRFeatureRequest> features,
                                                                 const Render::RenderFrontend &frontend,
                                                                 Editor::EditorViewportRendererVulkan &viewport,
                                                                 const NativeSessionCensus &session) {
            using Evidence = Result<ReprojectionEvidence>;
            const bool requested = std::ranges::any_of(features, [](const XRFeatureRequest &feature) {
                return feature.capability == XRCapability::SpaceWarp && feature.kind == XRFeatureRequirementKind::Optional;
            });
            if (!requested)
                return Evidence::Success({});
            const auto result = native.DiscoverReprojection(attempt);
            if (result.HasError())
                return Evidence::Failure(result.ErrorValue());
            const auto &offer = result.Value();
            return Evidence::Success(
                {offer.availabilityWithoutProducer, PrepareOptionalWriter(offer, attempt, frontend, viewport, session)});
        }

        struct OptionalProjectionEvidence final {
            XRCapabilityState hands;
            XRCapabilityState stage;
            XRCapabilityState reprojection;
            XRCapabilityState eyeGaze;
        };

        /** @brief Forms immutable capability evidence from validated native census and exact target support. */
        XRCapabilityDescriptor ProjectionCapabilities(const NativeSystemCensus &observed, const NativeSessionCensus &session,
                                                      const ProjectionTargets &targets, XRSystemId system, XRCapabilityRevision revision,
                                                      const XRSystemLimits &budget, OptionalProjectionEvidence optional) {
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
            descriptor.states[static_cast<std::size_t>(XRCapability::StageSpace)] = optional.stage;
            // Action/Input paths were actually prepared; frame/event/swapchain protocol dispatch was resolved above.
            // Native creation and execution still belong to the subsequent resource stages and real event/frame loop.
            for (auto capability :
                 {XRCapability::BooleanActions, XRCapability::FloatActions, XRCapability::Vector2Actions, XRCapability::PoseActions,
                  XRCapability::CanonicalInputProjection, XRCapability::PredictedFrames, XRCapability::SessionLossLifecycle})
                fact(capability, true);
            fact(XRCapability::ExternalColorTargets, targets.exactImages);
            descriptor.states[static_cast<std::size_t>(XRCapability::HandTracking)] = optional.hands;
            descriptor.states[static_cast<std::size_t>(XRCapability::SpaceWarp)] = optional.reprojection;
            descriptor.states[static_cast<std::size_t>(XRCapability::EyeGaze)] = optional.eyeGaze;
            return descriptor;
        }
    }  // namespace

    /** @copydoc OpenXRApplicationSession::ValidateProjectionDispatch */
    Result<void> OpenXRApplicationSession::ValidateProjectionDispatch(XrInstance instance) const {
        return ValidateProjectionProtocol(getProc_, instance);
    }

    /** @copydoc OpenXRApplicationSession::ValidatePackagedProjectionTargets */
    Result<void> OpenXRApplicationSession::ValidatePackagedProjectionTargets(const NativeSystemCensus &system,
                                                                             const NativeSessionCensus &session) {
        const auto targets = ObserveNativeProjectionTargets(system, session);
        if (targets.HasError())
            return Result<void>::Failure(targets.ErrorValue());
        if (!targets.Value().stereo)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "The packaged projection profile needs opaque stereo."));
        if (!targets.Value().colorAvailable)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnsupported, "The installed runtime does not offer RGBA8 UNORM stereo swapchains for Play."));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::ValidateProjectionPolicy */
    Result<void> OpenXRApplicationSession::ValidateProjectionPolicy(const OpenXRApplicationBindings &bindings) const {
        if (settings_.Profile() != XRFeatureProfile::Projection1_0)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto &budget = settings_.Limits();
        if (bindings.locomotion && std::holds_alternative<OpenXRArmSwingBinding>(bindings.locomotion->translation) &&
            budget.maximumDevices < 3)
            return Result<void>::Failure(
                MakeError(XRErrors::CapacityExceeded, "Arm swing requires device capacity for the head and both controllers."));
        if (const auto preparedActions = bindings.PreparedActionCount();
            preparedActions > budget.maximumActions || budget.maximumSpaces < 5 + bindings.poses.size() ||
            budget.maximumActions > XRActionBindingLimits::MaximumActions || budget.maximumDevices > 5 ||
            budget.maximumSpaces > XRHardLimits::MaximumSpaces)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::ValidateProjectionPreparation */
    Result<XRSystemId> OpenXRApplicationSession::ValidateProjectionPreparation(const Editor::EditorViewportRendererVulkan &viewport) const {
        using Outcome = Result<XRSystemId>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_ || !system_ || !sessionCensus_ || !input_.native || !input_.projection || !input_.bindings)
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
        if (auto protocol = ValidateProjectionDispatch(nativeSession.Value().instance); protocol.HasError())
            return Outcome::Failure(protocol.ErrorValue());
        const auto stillPrepared = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (stillPrepared.HasError())
            return Outcome::Failure(stillPrepared.ErrorValue());
        if (stillPrepared.Value().incarnation != nativeSession.Value().incarnation)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));

        return Outcome::Success(observedSystem.Value());
    }

    /** @copydoc OpenXRApplicationSession::PrepareProjection */
    Result<OpenXRProjectionDraft> OpenXRApplicationSession::PrepareProjection(const Render::RenderFrontend &frontend,
                                                                              Editor::EditorViewportRendererVulkan &viewport,
                                                                              const OpenXRApplicationBindings &bindings) {
        using Outcome = Result<OpenXRProjectionDraft>;
        const auto observedSystem = ValidateProjectionPreparation(viewport);
        if (observedSystem.HasError())
            return Outcome::Failure(observedSystem.ErrorValue());
        const auto &observed = *system_;
        const auto &session = *sessionCensus_;
        const auto targets = PrepareProjectionTargets(frontend, observed, session);
        if (targets.HasError())
            return Outcome::Failure(targets.ErrorValue());
        if (auto admitted = ValidateProjectionPolicy(bindings); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        const auto &budget = settings_.Limits();
        auto revision = XRCapabilityRevision::Create(lastCapabilityRevision_ + 1);
        if (revision.HasError())
            return Outcome::Failure(revision.ErrorValue());
        OptionalProjectionEvidence optional{input_.handAvailability, input_.stageAvailability, XRCapabilityState::Unsupported,
                                            EyeGazeAvailability()};
        const auto reprojection =
            PrepareOptionalReprojection(*native_, selection_.attempt, settings_.Features(), frontend, viewport, session);
        if (reprojection.HasError())
            return Outcome::Failure(reprojection.ErrorValue());
        optional.reprojection = reprojection.Value().observed;
        OpenXRProjectionDraft output{ProjectionCapabilities(observed, session, targets.Value(), observedSystem.Value(), revision.Value(),
                                                            budget, optional),
                                     targets.Value().color,
                                     {},
                                     {},
                                     reprojection.Value().writer};
        const auto quad = native_->DiscoverQuadViews(selection_.attempt, observed);
        if (quad.HasError())
            return Outcome::Failure(quad.ErrorValue());
        const auto quadState = quad.Value().state;
        output.capabilities.states[static_cast<std::size_t>(XRCapability::QuadViews)] =
            quadState == XRCapabilityState::Available ? XRCapabilityState::DependencyMissing : quadState;
        const auto foveation = native_->ReadExtensionAdmission(selection_.attempt, XR_HTC_FOVEATION_EXTENSION_NAME);
        if (foveation.HasError())
            return Outcome::Failure(foveation.ErrorValue());
        if (targets.Value().stereo && targets.Value().exactImages && foveation.Value().state == NativeExtensionState::Enabled)
            output.capabilities.states[static_cast<std::size_t>(XRCapability::FixedFoveation)] = XRCapabilityState::DependencyMissing;
        for (std::size_t eye = 0; eye < output.recommended.size(); ++eye) {
            output.recommended[eye] = {observed.primaryViews[eye].recommendedImageRectWidth,
                                       observed.primaryViews[eye].recommendedImageRectHeight};
            output.maximum[eye] = {observed.primaryViews[eye].maxImageRectWidth, observed.primaryViews[eye].maxImageRectHeight};
        }
        return Outcome::Success(std::move(output));
    }

    /** @copydoc OpenXRApplicationSession::FinalizeProjection */
    Result<OpenXRApplicationProjection> OpenXRApplicationSession::FinalizeProjection(OpenXRProjectionDraft draft,
                                                                                     const NativePreparedReprojectionProvider *provider) {
        using Outcome = Result<OpenXRApplicationProjection>;
        if (const auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!preflight_ || draft.capabilities.revision.Value() != lastCapabilityRevision_ + 1)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (provider && draft.writer)
            draft.capabilities.states[static_cast<std::size_t>(XRCapability::SpaceWarp)] = XRCapabilityState::Available;
        auto capabilities = XRCapabilitySnapshot::Create(draft.capabilities);
        if (capabilities.HasError())
            return Outcome::Failure(capabilities.ErrorValue());
        auto plan = AdmitXRProjectSettings(settings_, {authorities_.configuration.Snapshot().Revision(), *preflight_, selection_,
                                                       XRRendererCompatibility::Compatible, capabilities.Value(), draft.capabilities.system,
                                                       draft.capabilities.revision});
        if (plan.HasError())
            return Outcome::Failure(plan.ErrorValue());
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        ++lastCapabilityRevision_;
        return Outcome::Success({std::move(capabilities).Value(), std::move(plan).Value(), draft.color, draft.recommended, draft.maximum,
                                 std::move(draft.writer), settings_.WorldCanvasMinimumSpanPixels()});
    }
}  // namespace Horo::Application::Internal
