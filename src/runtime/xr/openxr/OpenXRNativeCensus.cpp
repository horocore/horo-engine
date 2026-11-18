#include "host/OpenXRNativeSession.h"

#include <algorithm>
#include <bit>
#include <format>
#include <iterator>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves native operation and numeric failure evidence at this private discovery boundary. */
        Error QueryFailure(const char *operation, const XrResult result) {
            return MakeError(result == XR_ERROR_RUNTIME_UNAVAILABLE ? XRErrors::RuntimeUnavailable : XRErrors::OperationUnavailable,
                             std::format("{} failed; native result={}", operation, static_cast<std::int32_t>(result)));
        }

        /** @brief Resolves an exact instance-scoped official census operation, never an ambient symbol. */
        template <typename Function> Result<Function> Resolve(const NativeDiscoveryBorrow &owner, const char *name) {
            PFN_xrVoidFunction function{};
            const auto result = owner.getProc(owner.instance, name, &function);
            if (XR_FAILED(result))
                return Result<Function>::Failure(QueryFailure(name, result));
            if (!function)
                return Result<Function>::Failure(MakeError(XRErrors::LoaderIncompatible, name));
            return Result<Function>::Success(std::bit_cast<Function>(function));
        }

        /** @brief Reads one bounded native list completely; oversized/replaced publications are rejected, never truncated. */
        template <typename Function, typename Element, std::size_t Size, typename... Prefix>
        Result<std::uint32_t> Enumerate(Function function, const char *name, std::array<Element, Size> &storage, Prefix... prefix) {
            std::uint32_t count{};
            auto result = function(prefix..., 0, &count, nullptr);
            if (XR_FAILED(result))
                return Result<std::uint32_t>::Failure(QueryFailure(name, result));
            if (count == 0 || count > storage.size())
                return Result<std::uint32_t>::Failure(MakeError(count == 0 ? XRErrors::OperationUnsupported : XRErrors::CapacityExceeded));
            const auto expected = count;
            result = function(prefix..., static_cast<std::uint32_t>(storage.size()), &count, storage.data());
            if (XR_FAILED(result))
                return Result<std::uint32_t>::Failure(QueryFailure(name, result));
            if (count != expected || count > storage.size())
                return Result<std::uint32_t>::Failure(
                    MakeError(XRErrors::OperationInvalid, "Native discovery list changed during enumeration"));
            return Result<std::uint32_t>::Success(count);
        }

        /** @brief Validates exact scalar publications without sorting away native runtime order. */
        template <typename Element, std::size_t Size> bool Unique(const std::array<Element, Size> &storage, const std::uint32_t count) {
            for (std::size_t index = 0; index < count; ++index)
                if (std::find(storage.begin(), storage.begin() + index, storage[index]) != storage.begin() + index)
                    return false;
            return true;
        }

        /** @brief Queries observed runtime identity and tracking/graphics system limits independently of any session. */
        Result<void> ReadProperties(const NativeDiscoveryBorrow &owner, NativeSystemCensus &census) {
            const auto instance = Resolve<PFN_xrGetInstanceProperties>(owner, "xrGetInstanceProperties");
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            auto result = instance.Value()(owner.instance, &census.runtime);
            if (XR_FAILED(result))
                return Result<void>::Failure(QueryFailure("xrGetInstanceProperties", result));
            const auto system = Resolve<PFN_xrGetSystemProperties>(owner, "xrGetSystemProperties");
            if (system.HasError())
                return Result<void>::Failure(system.ErrorValue());
            XrSystemHandTrackingPropertiesEXT hands{XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT};
            const bool handRoute = std::ranges::any_of(owner.enabledExtensions, [](const char *name) {
                return std::string_view{name} == XR_EXT_HAND_TRACKING_EXTENSION_NAME;
            });
            if (handRoute)
                census.system.next = &hands;
            result = system.Value()(owner.instance, owner.system, &census.system);
            const bool validChain = census.system.next == (handRoute ? &hands : nullptr) &&
                                    hands.type == XR_TYPE_SYSTEM_HAND_TRACKING_PROPERTIES_EXT && !hands.next &&
                                    hands.supportsHandTracking <= XR_TRUE;
            census.system.next = nullptr;
            if (XR_FAILED(result))
                return Result<void>::Failure(QueryFailure("xrGetSystemProperties", result));
            if (!validChain || census.system.systemId != owner.system || census.system.graphicsProperties.maxSwapchainImageWidth == 0 ||
                census.system.graphicsProperties.maxSwapchainImageHeight == 0 || census.system.graphicsProperties.maxLayerCount == 0 ||
                census.system.trackingProperties.orientationTracking > XR_TRUE ||
                census.system.trackingProperties.positionTracking > XR_TRUE ||
                std::ranges::find(census.runtime.runtimeName, '\0') == std::end(census.runtime.runtimeName) ||
                std::ranges::find(census.system.systemName, '\0') == std::end(census.system.systemName))
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native system properties"));
            if (handRoute && hands.supportsHandTracking == XR_TRUE)
                census.handTracking = XRCapabilityState::Available;
            return Result<void>::Success();
        }

        /** @brief Validates observed per-view native bounds; renderer/profile compatibility remains separate. */
        bool ValidView(const XrViewConfigurationView &view, const XrSystemGraphicsProperties &limits) {
            return view.type == XR_TYPE_VIEW_CONFIGURATION_VIEW && !view.next && view.recommendedImageRectWidth > 0 &&
                   view.recommendedImageRectHeight > 0 && view.recommendedSwapchainSampleCount > 0 &&
                   view.recommendedImageRectWidth <= view.maxImageRectWidth && view.recommendedImageRectHeight <= view.maxImageRectHeight &&
                   view.recommendedSwapchainSampleCount <= view.maxSwapchainSampleCount &&
                   view.maxImageRectWidth <= limits.maxSwapchainImageWidth && view.maxImageRectHeight <= limits.maxSwapchainImageHeight;
        }

        /** @brief Reads the complete primary stereo view and blend-mode publication from the actual selected system. */
        Result<void> ReadPrimary(const NativeDiscoveryBorrow &owner, NativeSystemCensus &census) {
            const auto views = Resolve<PFN_xrEnumerateViewConfigurationViews>(owner, "xrEnumerateViewConfigurationViews");
            if (views.HasError())
                return Result<void>::Failure(views.ErrorValue());
            for (auto &view : census.primaryViews)
                view.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
            const auto count = Enumerate(views.Value(), "xrEnumerateViewConfigurationViews", census.primaryViews, owner.instance,
                                         owner.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            census.primaryViewCount = count.Value();
            for (const auto &view : std::span{census.primaryViews}.first(census.primaryViewCount))
                if (!ValidView(view, census.system.graphicsProperties))
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native view publication"));
            const auto blends = Resolve<PFN_xrEnumerateEnvironmentBlendModes>(owner, "xrEnumerateEnvironmentBlendModes");
            if (blends.HasError())
                return Result<void>::Failure(blends.ErrorValue());
            const auto modes = Enumerate(blends.Value(), "xrEnumerateEnvironmentBlendModes", census.primaryBlendModes, owner.instance,
                                         owner.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO);
            if (modes.HasError())
                return Result<void>::Failure(modes.ErrorValue());
            census.primaryBlendModeCount = modes.Value();
            return Unique(census.primaryBlendModes, census.primaryBlendModeCount)
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate native blend mode"));
        }

        /** @brief Forms observed pre-session facts without claiming reference spaces, formats or engine capabilities. */
        Result<NativeSystemCensus> ReadSystem(const NativeDiscoveryBorrow &owner) {
            using Outcome = Result<NativeSystemCensus>;
            NativeSystemCensus census;
            if (const auto properties = ReadProperties(owner, census); properties.HasError())
                return Outcome::Failure(properties.ErrorValue());
            const auto enumerate = Resolve<PFN_xrEnumerateViewConfigurations>(owner, "xrEnumerateViewConfigurations");
            if (enumerate.HasError())
                return Outcome::Failure(enumerate.ErrorValue());
            const auto count =
                Enumerate(enumerate.Value(), "xrEnumerateViewConfigurations", census.configurations, owner.instance, owner.system);
            if (count.HasError())
                return Outcome::Failure(count.ErrorValue());
            census.configurationCount = count.Value();
            if (!Unique(census.configurations, census.configurationCount))
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate native view configuration"));
            if (const auto configurations = std::span{census.configurations}.first(census.configurationCount);
                std::ranges::find(configurations, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) == configurations.end())
                return Outcome::Success(census);  // Actual non-stereo discovery; the product must reject its unsupported profile.
            if (const auto primary = ReadPrimary(owner, census); primary.HasError())
                return Outcome::Failure(primary.ErrorValue());
            return Outcome::Success(census);
        }

        struct ReprojectionRoute final {
            NativeReprojectionRoute kind;
            std::string_view extension;
            std::uint32_t minimumVersion;
        };

        constexpr std::array ReprojectionRoutes{
            ReprojectionRoute{NativeReprojectionRoute::FrameSynthesisExt, XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME, 1},
            ReprojectionRoute{NativeReprojectionRoute::SpaceWarpFb, XR_FB_SPACE_WARP_EXTENSION_NAME, 1},
        };

        enum class OfferPriority : std::uint8_t {
            Absent,
            Rejected,
            Advertised,
            Enabled
        };

        /** @brief Keeps explicit enabled policy above advertisement. Equal offers keep the registered route order. */
        OfferPriority Priority(const ReprojectionRoute &route, const NativeExtensionAdmission &evidence) noexcept {
            if (evidence.state == NativeExtensionState::Enabled && evidence.discoveredVersion >= route.minimumVersion)
                return OfferPriority::Enabled;
            if (evidence.state == NativeExtensionState::Discovered && evidence.discoveredVersion >= route.minimumVersion)
                return OfferPriority::Advertised;
            return evidence.state != NativeExtensionState::Absent ? OfferPriority::Rejected : OfferPriority::Absent;
        }

        /** @brief Maps a rejected route without treating an advertised or enabled extension as a complete producer. */
        XRCapabilityState OfferState(const OfferPriority priority, const NativeExtensionAdmission &evidence) noexcept {
            if (priority >= OfferPriority::Advertised)
                return XRCapabilityState::DependencyMissing;
            switch (evidence.state) {
                case NativeExtensionState::Denied:
                    return XRCapabilityState::Denied;
                case NativeExtensionState::Unsupported:
                    return XRCapabilityState::Unsupported;
                case NativeExtensionState::DependencyMissing:
                    return XRCapabilityState::DependencyMissing;
                case NativeExtensionState::ProviderFailed:
                    return XRCapabilityState::TemporarilyUnavailable;
                default:
                    return XRCapabilityState::Incompatible;
            }
        }

        /** @brief Checks the native graphics range before an optional stereo configuration can use the dimensions. */
        bool FitsSwapchains(const Render::FramebufferExtent extent, const XrSystemGraphicsProperties &limits) noexcept {
            return extent.IsValid() && extent.width <= limits.maxSwapchainImageWidth && extent.height <= limits.maxSwapchainImageHeight;
        }

        /** @brief Queries enabled EXT sizing on each exact primary stereo view, preserving its native record order. */
        XRCapabilityState ReadExtentsExt(const NativeDiscoveryBorrow &owner, const XrSystemGraphicsProperties &limits,
                                         NativeReprojectionOffer &offer) {
            const auto enumerate = Resolve<PFN_xrEnumerateViewConfigurationViews>(owner, "xrEnumerateViewConfigurationViews");
            if (enumerate.HasError())
                return XRCapabilityState::TemporarilyUnavailable;
            std::array<XrViewConfigurationView, 2> views{};
            std::array<XrFrameSynthesisConfigViewEXT, views.size()> synthesis{};
            for (std::size_t index = 0; index < views.size(); ++index) {
                synthesis[index].type = XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT;
                views[index] = {XR_TYPE_VIEW_CONFIGURATION_VIEW, &synthesis[index]};
            }
            std::uint32_t count{};
            const auto result = enumerate.Value()(owner.instance, owner.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                  static_cast<std::uint32_t>(views.size()), &count, views.data());
            if (XR_FAILED(result))
                return result == XR_ERROR_SIZE_INSUFFICIENT ? XRCapabilityState::Incompatible : XRCapabilityState::TemporarilyUnavailable;
            if (count != views.size())
                return XRCapabilityState::Incompatible;
            std::array<Render::FramebufferExtent, views.size()> extents{};
            for (std::size_t index = 0; index < views.size(); ++index) {
                const auto &native = synthesis[index];
                extents[index] = {native.recommendedMotionVectorImageRectWidth, native.recommendedMotionVectorImageRectHeight};
                if (views[index].type != XR_TYPE_VIEW_CONFIGURATION_VIEW || views[index].next != &native ||
                    native.type != XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT || native.next || !FitsSwapchains(extents[index], limits))
                    return XRCapabilityState::Incompatible;
            }
            offer.extents = extents;
            return XRCapabilityState::DependencyMissing;
        }

        /** @brief Queries sizing only for a route actually enabled on this instance. Neither route grants a writer. */
        XRCapabilityState ReadReprojectionExtents(const NativeDiscoveryBorrow &owner, NativeReprojectionOffer &offer) {
            const auto get = Resolve<PFN_xrGetSystemProperties>(owner, "xrGetSystemProperties");
            if (get.HasError())
                return XRCapabilityState::TemporarilyUnavailable;
            const bool legacy = offer.route == NativeReprojectionRoute::SpaceWarpFb;
            XrSystemSpaceWarpPropertiesFB spaceWarp{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
            XrSystemProperties system{XR_TYPE_SYSTEM_PROPERTIES, legacy ? &spaceWarp : nullptr};
            const auto result = get.Value()(owner.instance, owner.system, &system);
            if (XR_FAILED(result))
                return XRCapabilityState::TemporarilyUnavailable;
            if (system.type != XR_TYPE_SYSTEM_PROPERTIES || system.systemId != owner.system ||
                system.next != (legacy ? &spaceWarp : nullptr) || !system.graphicsProperties.maxSwapchainImageWidth ||
                !system.graphicsProperties.maxSwapchainImageHeight)
                return XRCapabilityState::Incompatible;
            if (!legacy)
                return ReadExtentsExt(owner, system.graphicsProperties, offer);
            const Render::FramebufferExtent extent{spaceWarp.recommendedMotionVectorImageRectWidth,
                                                   spaceWarp.recommendedMotionVectorImageRectHeight};
            if (spaceWarp.type != XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB || spaceWarp.next ||
                !FitsSwapchains(extent, system.graphicsProperties))
                return XRCapabilityState::Incompatible;
            offer.extents.fill(extent);
            return XRCapabilityState::DependencyMissing;
        }

        /** @brief Reads only session-dependent publications from the exact existing unpublished native session. */
        Result<NativeSessionCensus> ReadSession(const NativeDiscoveryBorrow &owner, const XrSession session) {
            using Outcome = Result<NativeSessionCensus>;
            NativeSessionCensus census;
            const auto spaces = Resolve<PFN_xrEnumerateReferenceSpaces>(owner, "xrEnumerateReferenceSpaces");
            if (spaces.HasError())
                return Outcome::Failure(spaces.ErrorValue());
            const auto spaceCount = Enumerate(spaces.Value(), "xrEnumerateReferenceSpaces", census.spaces, session);
            if (spaceCount.HasError())
                return Outcome::Failure(spaceCount.ErrorValue());
            census.spaceCount = spaceCount.Value();
            const auto formats = Resolve<PFN_xrEnumerateSwapchainFormats>(owner, "xrEnumerateSwapchainFormats");
            if (formats.HasError())
                return Outcome::Failure(formats.ErrorValue());
            const auto formatCount = Enumerate(formats.Value(), "xrEnumerateSwapchainFormats", census.formats, session);
            if (formatCount.HasError())
                return Outcome::Failure(formatCount.ErrorValue());
            census.formatCount = formatCount.Value();
            if (!Unique(census.spaces, census.spaceCount) || !Unique(census.formats, census.formatCount))
                return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Duplicate native session publication"));
            return Outcome::Success(census);
        }
    }  // namespace

    /** @copydoc OpenXRNativeSession::DiscoverSystem */
    Result<NativeSystemCensus> OpenXRNativeSession::DiscoverSystem(const XRLoaderPreflightAttempt attempt) const {
        const auto owner = BorrowDiscovery(attempt);
        if (owner.HasError())
            return Result<NativeSystemCensus>::Failure(owner.ErrorValue());
        auto census = ReadSystem(owner.Value());
        if (const auto current = BorrowDiscovery(attempt); current.HasError())
            return Result<NativeSystemCensus>::Failure(current.ErrorValue());
        return census;
    }

    /** @copydoc OpenXRNativeSession::DiscoverReprojection */
    Result<NativeReprojectionOffer> OpenXRNativeSession::DiscoverReprojection(const XRLoaderPreflightAttempt attempt) const {
        using Outcome = Result<NativeReprojectionOffer>;
        const auto owner = BorrowDiscovery(attempt);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        NativeReprojectionOffer offer;
        auto priority = OfferPriority::Absent;
        for (const auto &route : ReprojectionRoutes) {
            const auto admission = ReadExtensionAdmission(attempt, route.extension);
            if (admission.HasError())
                return Outcome::Failure(admission.ErrorValue());
            const auto &evidence = admission.Value();
            const auto next = Priority(route, evidence);
            if (next <= priority)
                continue;
            priority = next;
            offer.route = route.kind;
            offer.version = evidence.discoveredVersion;
            offer.availabilityWithoutProducer = OfferState(next, evidence);
        }
        if (priority == OfferPriority::Enabled)
            offer.availabilityWithoutProducer = ReadReprojectionExtents(owner.Value(), offer);
        if (const auto current = BorrowDiscovery(attempt); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Success(offer);
    }

    /** @copydoc OpenXRNativeSession::DiscoverSession */
    Result<NativeSessionCensus> OpenXRNativeSession::DiscoverSession(const NativeSessionPreparation &request) const {
        const auto session = BorrowPreparedSession(request);
        if (session.HasError())
            return Result<NativeSessionCensus>::Failure(session.ErrorValue());
        const auto owner = BorrowDiscovery(request.activeAttempt);
        if (owner.HasError())
            return Result<NativeSessionCensus>::Failure(owner.ErrorValue());
        auto census = ReadSession(owner.Value(), session.Value().session);
        if (const auto current = CheckPreparation(request); current.HasError())
            return Result<NativeSessionCensus>::Failure(current.ErrorValue());
        return census;
    }
}  // namespace Horo::XR::OpenXRInternal
