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
            result = system.Value()(owner.instance, owner.system, &census.system);
            if (XR_FAILED(result))
                return Result<void>::Failure(QueryFailure("xrGetSystemProperties", result));
            if (census.system.systemId != owner.system || census.system.graphicsProperties.maxSwapchainImageWidth == 0 ||
                census.system.graphicsProperties.maxSwapchainImageHeight == 0 || census.system.graphicsProperties.maxLayerCount == 0 ||
                census.system.trackingProperties.orientationTracking > XR_TRUE ||
                census.system.trackingProperties.positionTracking > XR_TRUE ||
                std::ranges::find(census.runtime.runtimeName, '\0') == std::end(census.runtime.runtimeName) ||
                std::ranges::find(census.system.systemName, '\0') == std::end(census.system.systemName))
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Malformed native system properties"));
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
