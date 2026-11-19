#include "host/OpenXRNativeSession.h"
#include "host/OpenXRSwapchain.h"

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

        /** @brief Identity of an extension already enabled on the selected instance. */
        bool Enabled(const NativeDiscoveryBorrow &owner, std::string_view extension) {
            return std::ranges::find(owner.enabledExtensions, extension, [](const char *name) {
                return std::string_view{name};
            }) != owner.enabledExtensions.end();
        }

        /** @brief Observes cold system support independently of pose actions and any privacy grant. */
        XRCapabilityState EyeGazeProvider(PFN_xrGetSystemProperties query, const NativeDiscoveryBorrow &owner) {
            XrSystemEyeGazeInteractionPropertiesEXT eyes{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
            XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &eyes};
            const auto result = query(owner.instance, owner.system, &properties);
            if (result == XR_ERROR_PERMISSION_INSUFFICIENT)
                return XRCapabilityState::Denied;
            if (result != XR_SUCCESS)
                return XRCapabilityState::Unavailable;
            if (properties.systemId != owner.system || properties.type != XR_TYPE_SYSTEM_PROPERTIES || properties.next != &eyes ||
                eyes.type != XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT || eyes.next || eyes.supportsEyeGazeInteraction > XR_TRUE)
                return XRCapabilityState::Incompatible;
            return eyes.supportsEyeGazeInteraction == XR_TRUE ? XRCapabilityState::Available : XRCapabilityState::Unsupported;
        }

        /** @brief Maps optional native discovery failures without disabling the baseline XR profile. */
        XRCapabilityState SpatialCensusFailure(const XrResult result) noexcept {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return XRCapabilityState::Denied;
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                case XR_ERROR_FEATURE_UNSUPPORTED:
                    return XRCapabilityState::Unsupported;
                case XR_ERROR_SIZE_INSUFFICIENT:
                    return XRCapabilityState::Incompatible;
                default:
                    return XRCapabilityState::Unavailable;
            }
        }

        /** @brief Reads only spatial capability metadata before any session or authorization exists. */
        NativePlaneCensus PlaneProvider(const NativeDiscoveryBorrow &owner) {
            using enum XRCapabilityState;
            if (!Enabled(owner, XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME))
                return {};
            if (!Enabled(owner, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME) || !Enabled(owner, XR_EXT_FUTURE_EXTENSION_NAME))
                return {DependencyMissing};

            const auto capabilities = Resolve<PFN_xrEnumerateSpatialCapabilitiesEXT>(owner, "xrEnumerateSpatialCapabilitiesEXT");
            const auto components =
                Resolve<PFN_xrEnumerateSpatialCapabilityComponentTypesEXT>(owner, "xrEnumerateSpatialCapabilityComponentTypesEXT");
            if (capabilities.HasError() || components.HasError())
                return {Incompatible};

            constexpr std::uint32_t maximumNativeFacts = 32;
            std::array<XrSpatialCapabilityEXT, maximumNativeFacts> available{};
            std::uint32_t count{};
            auto result = capabilities.Value()(owner.instance, owner.system, maximumNativeFacts, &count, available.data());
            if (result != XR_SUCCESS)
                return {SpatialCensusFailure(result)};
            if (count > available.size() || !Unique(available, count))
                return {Incompatible};
            const auto publishedCapabilities = std::span{available}.first(count);
            if (std::ranges::find(publishedCapabilities, XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT) == publishedCapabilities.end())
                return {};

            std::array<XrSpatialComponentTypeEXT, maximumNativeFacts> types{};
            XrSpatialCapabilityComponentTypesEXT publication{XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT, nullptr, maximumNativeFacts, 0,
                                                             types.data()};
            result = components.Value()(owner.instance, owner.system, XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT, &publication);
            if (result != XR_SUCCESS)
                return {SpatialCensusFailure(result)};
            count = publication.componentTypeCountOutput;
            if (publication.type != XR_TYPE_SPATIAL_CAPABILITY_COMPONENT_TYPES_EXT || publication.next ||
                publication.componentTypeCapacityInput != maximumNativeFacts || publication.componentTypes != types.data() ||
                count > types.size() || !Unique(types, count))
                return {Incompatible};
            const auto published = std::span{types}.first(count);
            if (std::ranges::find(published, XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT) == published.end())
                return {Incompatible};
            return {Available, std::ranges::find(published, XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT) != published.end()};
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
            const bool handRoute = Enabled(owner, XR_EXT_HAND_TRACKING_EXTENSION_NAME);
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
            if (Enabled(owner, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME))
                census.eyeGazeProvider = EyeGazeProvider(system.Value(), owner);
            census.planes = PlaneProvider(owner);
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

    /** @copydoc OpenXRNativeSession::BorrowFeatureDiscovery */
    Result<NativeDiscoveryBorrow> OpenXRNativeSession::BorrowFeatureDiscovery(const XRLoaderPreflightAttempt attempt,
                                                                              const std::optional<XRSessionId> &session) const {
        using Outcome = Result<NativeDiscoveryBorrow>;
        if (!session)
            return BorrowDiscovery(attempt);
        if (controlThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!discovery_ || discovery_->phase != DiscoveryPhase::Active || discovery_->selection.attempt != attempt)
            return Outcome::Failure(MakeError(XRErrors::LoaderPreflightStale));
        const auto owner = Borrow(*session);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        return Outcome::Success({owner.Value().instance,
                                 owner.Value().system,
                                 dispatch_.getProc,
                                 {names_.extensions.data(), names_.extensionCount},
                                 discovery_->acceptedApi});
    }

    /** @copydoc OpenXRNativeSession::DiscoverPlaneProvider */
    Result<NativePlaneCensus> OpenXRNativeSession::DiscoverPlaneProvider(const XRLoaderPreflightAttempt attempt,
                                                                         const XRSessionId &session) const {
        using Outcome = Result<NativePlaneCensus>;
        const auto owner = BorrowFeatureDiscovery(attempt, session);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        const auto feature = PlaneProvider(owner.Value());
        if (const auto current = BorrowFeatureDiscovery(attempt, session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Success(feature);
    }

    /** @copydoc OpenXRNativeSession::ReprojectionExtensionRequests */
    std::span<const NativeExtensionRequest> OpenXRNativeSession::ReprojectionExtensionRequests() noexcept {
        static constexpr auto requests = [] {
            std::array<NativeExtensionRequest, ReprojectionRoutes.size()> selected{};
            for (std::size_t index = 0; index < selected.size(); ++index)
                selected[index] = {.name = ReprojectionRoutes[index].extension, .minimumVersion = ReprojectionRoutes[index].minimumVersion};
            return selected;
        }();
        return requests;
    }

    /** @copydoc OpenXRNativeSession::DiscoverReprojection */
    Result<NativeReprojectionOffer> OpenXRNativeSession::DiscoverReprojection(const XRLoaderPreflightAttempt attempt,
                                                                              const std::optional<XRSessionId> session) const {
        using Outcome = Result<NativeReprojectionOffer>;
        const auto owner = BorrowFeatureDiscovery(attempt, session);
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
        if (const auto current = BorrowFeatureDiscovery(attempt, session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Success(offer);
    }

    /** @copydoc OpenXRNativeSession::RegisterReprojectionProvider */
    Result<NativeReprojectionProvider> OpenXRNativeSession::RegisterReprojectionProvider(const XRSessionId &session,
                                                                                         const XRLoaderPreflightAttempt attempt) {
        using Outcome = Result<NativeReprojectionProvider>;
        const auto owner = BorrowFeatureDiscovery(attempt, session);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (retainedPlan_->Decision(XRCapability::SpaceWarp).state != XRFeatureDecisionState::OptionalEnabled)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto observed = DiscoverReprojection(attempt, session);
        if (observed.HasError())
            return Outcome::Failure(observed.ErrorValue());
        const auto &offer = observed.Value();
        const auto route = std::ranges::find(ReprojectionRoutes, offer.route, &ReprojectionRoute::kind);
        if (route == ReprojectionRoutes.end() || !std::ranges::all_of(offer.extents, [](const auto &extent) {
            return extent.IsValid();
        }))
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        auto registered = RegisterFeatureProvider(session, {route->extension, "xrEndFrame"});
        if (registered.HasError())
            return Outcome::Failure(registered.ErrorValue());
        return Outcome::Success(NativeReprojectionProvider{std::move(registered).Value(), attempt, offer});
    }

    /** @copydoc OpenXRNativeSession::PrepareReprojectionProvider */
    Result<NativePreparedReprojectionProvider> OpenXRNativeSession::PrepareReprojectionProvider(const NativeSessionPreparation &request,
                                                                                                const OpenXRSwapchain &motion,
                                                                                                const OpenXRSwapchain &depth) const {
        using Outcome = Result<NativePreparedReprojectionProvider>;
        const auto owner = BorrowPreparedSession(request);
        if (owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        const auto discovered = DiscoverReprojection(request.activeAttempt);
        if (discovered.HasError())
            return Outcome::Failure(discovered.ErrorValue());
        const auto &offer = discovered.Value();
        const auto &session = owner.Value();
        const NativeReprojectionAllocation selected{request.activeAttempt, offer.route, offer.extents};
        const auto matches = [&](const OpenXRSwapchain &chain, const NativeSwapchainRole role) {
            if (chain.session_ != this || chain.phase_ != NativeSwapchainPhase::Prepared || !chain.stagedReprojection_ ||
                chain.stagedReprojection_->incarnation != session.incarnation)
                return false;
            const auto &allocation = chain.stagedReprojection_->allocation;
            return allocation.role == role && allocation.selection == selected;
        };
        if (offer.availabilityWithoutProducer != XRCapabilityState::DependencyMissing || !matches(motion, NativeSwapchainRole::Motion) ||
            !matches(depth, NativeSwapchainRole::ReprojectionDepth) || motion.native_ == depth.native_ ||
            motion.stagedReprojection_->allocation.slot.index == depth.stagedReprojection_->allocation.slot.index)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const std::array allocations{motion.native_, depth.native_};

        const auto discovery = BorrowDiscovery(request.activeAttempt);
        if (discovery.HasError())
            return Outcome::Failure(discovery.ErrorValue());
        const auto end = Resolve<PFN_xrEndFrame>(discovery.Value(), "xrEndFrame");
        if (end.HasError())
            return Outcome::Failure(end.ErrorValue());
        const auto current = BorrowPreparedSession(request);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (current.Value().incarnation != session.incarnation || allocations != std::array{motion.native_, depth.native_} ||
            !matches(motion, NativeSwapchainRole::Motion) || !matches(depth, NativeSwapchainRole::ReprojectionDepth))
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success(NativePreparedReprojectionProvider{request.activeAttempt,
                                                                   session.incarnation,
                                                                   offer,
                                                                   {&motion, &depth},
                                                                   allocations,
                                                                   std::bit_cast<PFN_xrVoidFunction>(end.Value())});
    }

    /** @copydoc OpenXRNativeSession::RegisterPreparedReprojectionProvider */
    Result<NativeReprojectionProvider> OpenXRNativeSession::RegisterPreparedReprojectionProvider(
        const XRSessionId &session, const NativePreparedReprojectionProvider &prepared, const OpenXRSwapchain &motion,
        const OpenXRSwapchain &depth) {
        using Outcome = Result<NativeReprojectionProvider>;
        if (const auto discovery = BorrowFeatureDiscovery(prepared.attempt_, session); discovery.HasError())
            return Outcome::Failure(discovery.ErrorValue());
        const auto current = Borrow(session);
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (prepared.incarnation_ != current.Value().incarnation ||
            prepared.owners_ != std::array<const OpenXRSwapchain *, 2>{&motion, &depth} ||
            prepared.allocations_ != std::array{motion.native_, depth.native_})
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const NativeReprojectionAllocation selected{prepared.attempt_, prepared.offer_.route, prepared.offer_.extents};
        const auto adopted = [&](const OpenXRSwapchain &chain, const NativeSwapchainRole role) {
            return chain.phase_ == NativeSwapchainPhase::Available && chain.request_.target.session == session &&
                   chain.request_.role == role && chain.request_.reprojection == selected;
        };
        if (!adopted(motion, NativeSwapchainRole::Motion) || !adopted(depth, NativeSwapchainRole::ReprojectionDepth))
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (retainedPlan_->Decision(XRCapability::SpaceWarp).state != XRFeatureDecisionState::OptionalEnabled)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto route = std::ranges::find(ReprojectionRoutes, prepared.offer_.route, &ReprojectionRoute::kind);
        if (route == ReprojectionRoutes.end())
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        auto registered = RegisterFeatureProvider(session, {route->extension, "xrEndFrame"},
                                                  PreparedFeatureDispatch{prepared.incarnation_, prepared.end_});
        if (registered.HasError())
            return Outcome::Failure(registered.ErrorValue());
        return Outcome::Success(NativeReprojectionProvider{std::move(registered).Value(), prepared.attempt_, prepared.offer_});
    }

    /** @copydoc OpenXRNativeSession::BorrowReprojectionProvider */
    Result<NativeReprojectionProviderBorrow> OpenXRNativeSession::BorrowReprojectionProvider(
        const NativeReprojectionProvider &provider) const {
        using Outcome = Result<NativeReprojectionProviderBorrow>;
        const auto registered = BorrowFeatureProvider(provider.handle_);
        if (registered.HasError())
            return Outcome::Failure(registered.ErrorValue());
        return Outcome::Success({provider.attempt_, provider.offer_, registered.Value().native.session,
                                 std::bit_cast<PFN_xrEndFrame>(registered.Value().function)});
    }

    /** @copydoc OpenXRNativeSession::RevokeReprojectionProvider */
    void OpenXRNativeSession::RevokeReprojectionProvider(const NativeReprojectionProvider &provider) noexcept {
        RevokeFeatureProvider(provider.handle_);
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
