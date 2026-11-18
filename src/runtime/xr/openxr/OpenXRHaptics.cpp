#include "host/OpenXRActionBindings.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Lookup slot only after matching one of the baseline controller roles. */
        std::optional<std::size_t> HapticHand(const XRTrackedDeviceRole role) noexcept {
            if (role == XRTrackedDeviceRole::LeftController)
                return 0;
            if (role == XRTrackedDeviceRole::RightController)
                return 1;
            return std::nullopt;
        }

        constexpr std::array HapticPaths{"/user/hand/left/output/haptic", "/user/hand/right/output/haptic"};
        constexpr std::array HapticNames{"horo_vibration_left", "horo_vibration_right"};

        /** @brief Preserves native output diagnostics, including positive no-submission results. */
        Error HapticFailure(const char *phase, const XrResult result) {
            const auto &code = result == XR_ERROR_PATH_UNSUPPORTED ? XRErrors::OperationUnsupported : XRErrors::OperationUnavailable;
            return MakeError(code, std::format("OpenXR {} returned native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Stops could not reach hardware and no command may be sent during parent retirement. */
        bool OutputUnavailable(const XrResult result) noexcept {
            return result == XR_SESSION_NOT_FOCUSED || result == XR_SESSION_LOSS_PENDING || result == XR_ERROR_SESSION_LOST ||
                   result == XR_ERROR_INSTANCE_LOST;
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::ValidateHapticPaths */
    Result<std::uint32_t> OpenXRActionBindings::ValidateHapticPaths(const NativeActionBindingRequest &request,
                                                                    const std::uint32_t maximumActions) const {
        using Count = Result<std::uint32_t>;
        if (request.hapticPaths.size() > XRActionBindingLimits::MaximumProfiles * HapticPaths.size())
            return Count::Failure(MakeError(XRErrors::CapacityExceeded));
        std::array<const NativeHapticActionPath *, HapticPaths.size()> routes{};
        std::uint32_t count{};
        for (std::size_t index = 0; index < request.hapticPaths.size(); ++index) {
            const auto &path = request.hapticPaths[index];
            const auto hand = HapticHand(path.role);
            if (!hand || path.path != HapticPaths[*hand] || path.label.empty() || path.label.size() >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE ||
                path.label.find('\0') != std::string_view::npos ||
                std::ranges::find(request.profiles, path.profile, &NativeInteractionProfilePath::profile) == request.profiles.end() ||
                std::ranges::find(request.schema.actions, path.set, &XRActionDeclaration::set) == request.schema.actions.end())
                return Count::Failure(MakeError(XRErrors::OperationInvalid, "Invalid registered OpenXR vibration route."));
            const auto *previous = routes[*hand];
            if (previous) {
                if (previous->set != path.set || previous->label != path.label ||
                    std::ranges::any_of(request.hapticPaths.first(index), [&](const auto &candidate) {
                    return candidate.role == path.role && candidate.profile == path.profile;
                }))
                    return Count::Failure(MakeError(XRErrors::OperationInvalid, "Conflicting OpenXR vibration routes."));
            } else {
                routes[*hand] = &path;
                ++count;
            }
        }
        if (count > maximumActions || request.schema.actions.size() > maximumActions - count)
            return Count::Failure(MakeError(XRErrors::CapacityExceeded));
        return Count::Success(count);
    }

    /** @copydoc OpenXRActionBindings::CreateHapticActions */
    Result<void> OpenXRActionBindings::CreateHapticActions(const std::span<const NativeHapticActionPath> paths) {
        for (const auto &path : paths) {
            const auto hand = *HapticHand(path.role);
            auto &action = haptics_[hand];
            action.profiles[action.profileCount++] = path.profile;
            if (action.native != XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.stringToPath(native_.instance, HapticPaths[hand], &action.source);
            if (result != XR_SUCCESS || action.source == XR_NULL_PATH)
                return Result<void>::Failure(
                    HapticFailure("resolve vibration path", result == XR_SUCCESS ? XR_ERROR_PATH_INVALID : result));

            const auto set = std::ranges::find(sets_, path.set, &Set::id);
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            std::ranges::copy(std::string_view{HapticNames[hand]}, info.actionName);
            std::ranges::copy(path.label, info.localizedActionName);
            info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
            info.countSubactionPaths = 1;
            info.subactionPaths = &hands_[hand];
            const auto created = dispatch_.createAction(set->native, &info, &action.native);
            if (created != XR_SUCCESS || action.native == XR_NULL_HANDLE)
                return Result<void>::Failure(
                    HapticFailure("create vibration action", created == XR_SUCCESS ? XR_ERROR_RUNTIME_FAILURE : created));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::SuggestHaptics */
    Result<std::uint32_t> OpenXRActionBindings::SuggestHaptics(const Profile &profile,
                                                               const std::span<XrActionSuggestedBinding> output) const {
        std::uint32_t count{};
        for (const auto &action : haptics_) {
            const auto catalog = std::span{action.profiles}.first(action.profileCount);
            if (std::ranges::find(catalog, profile.id) == catalog.end())
                continue;
            if (count == output.size())
                return Result<std::uint32_t>::Failure(MakeError(XRErrors::CapacityExceeded));
            output[count++] = {action.native, action.source};
        }
        return Result<std::uint32_t>::Success(count);
    }

    /** @copydoc OpenXRActionBindings::ValidateHapticSource */
    Result<void> OpenXRActionBindings::ValidateHapticSource(const std::size_t hand) const {
        const auto &action = haptics_[hand];
        if (action.native == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported, "No vibration output is admitted for this controller."));
        XrInteractionProfileState observed{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (const auto result = dispatch_.profile(native_.session, hands_[hand], &observed); result != XR_SUCCESS)
            return Result<void>::Failure(HapticFailure("observe vibration profile", result));
        const auto profile = std::ranges::find(profiles_, observed.interactionProfile, &Profile::native);
        const auto catalog = std::span{action.profiles}.first(action.profileCount);
        if (profile == profiles_.end() || std::ranges::find(catalog, profile->id) == catalog.end())
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnsupported, "The current interaction profile has no admitted vibration."));
        if (auto current = ValidateOwner(); current.HasError())
            return current;
        const XrBoundSourcesForActionEnumerateInfo request{XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO, nullptr, action.native};
        XrPath source{};
        std::uint32_t count{};
        const auto result = dispatch_.boundHaptics(native_.session, &request, 1, &count, &source);
        if (result == XR_ERROR_SIZE_INSUFFICIENT || (result == XR_SUCCESS && count > 1))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded, "Vibration output has multiple bound native sources."));
        if (result != XR_SUCCESS)
            return Result<void>::Failure(HapticFailure("observe bound vibration output", result));
        if (count != 1 || source != action.source)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnsupported, "The admitted vibration output is not bound by the runtime."));
        return ValidateOwner();
    }

    /** @copydoc OpenXRActionBindings::ApplyHaptic */
    Result<void> OpenXRActionBindings::ApplyHaptic(const XRSessionId &session, const XRTrackedDeviceRole role,
                                                   const NativeHapticVibration vibration) {
        if (auto lane = ControlLane(); lane.HasError())
            return lane;
        if (sampling_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{sampling_};
        const auto current = Current(session);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto hand = HapticHand(role);
        if (!hand || !std::isfinite(vibration.amplitude) || vibration.amplitude < 0.0F || vibration.amplitude > 1.0F ||
            vibration.duration.ToNanoseconds() <= 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (auto available = ValidateHapticSource(*hand); available.HasError())
            return available;
        auto &action = haptics_[*hand];
        const XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action.native, hands_[*hand]};
        const XrHapticVibration effect{XR_TYPE_HAPTIC_VIBRATION, nullptr, vibration.duration.ToNanoseconds(), XR_FREQUENCY_UNSPECIFIED,
                                       vibration.amplitude};
        const auto result = dispatch_.applyHaptic(native_.session, &info, reinterpret_cast<const XrHapticBaseHeader *>(&effect));
        if (result == XR_SUCCESS || result == XR_SESSION_LOSS_PENDING)
            action.submitted = true;
        if (result != XR_SUCCESS)
            return Result<void>::Failure(HapticFailure("submit vibration", result));
        return ValidateOwner();
    }

    /** @copydoc OpenXRActionBindings::StopNativeHaptic */
    Result<void> OpenXRActionBindings::StopNativeHaptic(const std::size_t hand, const bool retiring) {
        auto &action = haptics_[hand];
        if (!action.submitted)
            return Result<void>::Success();
        const XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, action.native, hands_[hand]};
        const auto result = dispatch_.stopHaptic(native_.session, &info);
        if (result != XR_SUCCESS && !(retiring && OutputUnavailable(result)))
            return Result<void>::Failure(HapticFailure("stop vibration", result));
        action.submitted = false;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::StopHaptic */
    Result<void> OpenXRActionBindings::StopHaptic(const XRSessionId &session, const XRTrackedDeviceRole role) {
        if (auto lane = ControlLane(); lane.HasError())
            return lane;
        if (sampling_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const Operation operation{sampling_};
        const auto current = Current(session);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto hand = HapticHand(role);
        if (!hand)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (haptics_[*hand].native == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (auto stopped = StopNativeHaptic(*hand, false); stopped.HasError())
            return stopped;
        return ValidateOwner();
    }

    /** @copydoc OpenXRActionBindings::CloseHaptics */
    Result<void> OpenXRActionBindings::CloseHaptics() {
        for (std::size_t hand = 0; hand < haptics_.size(); ++hand)
            if (auto stopped = StopNativeHaptic(hand, true); stopped.HasError())
                return stopped;
        for (auto &action : haptics_) {
            if (action.native == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroyAction(action.native);
            if (XR_FAILED(result))
                return Result<void>::Failure(HapticFailure("destroy vibration action", result));
            action.native = XR_NULL_HANDLE;
        }
        haptics_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
