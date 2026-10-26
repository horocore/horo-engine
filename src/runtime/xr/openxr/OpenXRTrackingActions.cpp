#include "host/OpenXRActionBindings.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves the exact failed native tracking operation. */
        Error PoseActionFailure(const char *phase, const XrResult result) {
            return MakeError(XRErrors::OperationUnavailable,
                             std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Accepts only the registered controller grip/aim vocabulary, never arbitrary runtime paths. */
        bool PosePath(const NativePoseActionPath &path) {
            using enum XRTrackedPoseKind;
            const bool left = path.role == XRTrackedDeviceRole::LeftController;
            if ((!left && path.role != XRTrackedDeviceRole::RightController) || (path.kind != Grip && path.kind != Aim))
                return false;
            if (left) {
                if (path.kind == Grip)
                    return path.path == "/user/hand/left/input/grip/pose";
                return path.path == "/user/hand/left/input/aim/pose";
            }
            if (path.kind == Grip)
                return path.path == "/user/hand/right/input/grip/pose";
            return path.path == "/user/hand/right/input/aim/pose";
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::ValidatePosePaths */
    Result<void> OpenXRActionBindings::ValidatePosePaths(const NativeActionBindingRequest &request,
                                                         const std::uint32_t maximumActions) const {
        if (!request.tracking && request.posePaths.empty())
            return Result<void>::Success();
        if (!request.tracking || !request.tracking->origin.IsValid() ||
            (request.tracking->base != XRSpaceKind::Local && request.tracking->base != XRSpaceKind::Stage) ||
            request.posePaths.size() > XRActionBindingLimits::MaximumProfiles * 4)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (request.tracking->worldFromBase) {
            const auto &world = *request.tracking->worldFromBase;
            const auto rotation = world.rotation.TryNormalized();
            if (!Math::IsFinite(world.translation) || !Math::IsFinite(world.scale) || std::abs(world.scale.x - 1.0F) > 0.00001F ||
                std::abs(world.scale.y - 1.0F) > 0.00001F || std::abs(world.scale.z - 1.0F) > 0.00001F || rotation.HasError())
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            if (rotation.Value().Inverse().TryRotate(-world.translation).HasError())
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        std::size_t unique = 0;
        for (std::size_t index = 0; index < request.posePaths.size(); ++index) {
            const auto &path = request.posePaths[index];
            const auto previous = request.posePaths.first(index);
            if (!PosePath(path) || path.label.empty() || path.label.size() >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE ||
                path.label.find('\0') != std::string_view::npos ||
                std::ranges::find(request.profiles, path.profile, &NativeInteractionProfilePath::profile) == request.profiles.end() ||
                std::ranges::find(request.schema.actions, path.set, &XRActionDeclaration::set) == request.schema.actions.end() ||
                std::ranges::any_of(previous, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind &&
                       (entry.profile == path.profile || entry.set != path.set || entry.label != path.label);
            }))
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            if (std::ranges::none_of(previous, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind;
            }))
                ++unique;
        }
        if (unique > 4 || request.schema.actions.size() + unique > maximumActions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::CreatePoseActions */
    Result<void> OpenXRActionBindings::CreatePoseActions(const std::span<const NativePoseActionPath> paths) {
        for (const auto &path : paths) {
            if (std::ranges::any_of(tracking_.actions, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind;
            }))
                continue;
            const auto set = std::ranges::find(sets_, path.set, &Set::id);
            const std::size_t role = path.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            const auto name = std::format("horo_pose_{}", tracking_.actions.size());
            std::ranges::copy(name, info.actionName);
            std::ranges::copy(path.label, info.localizedActionName);
            info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            info.countSubactionPaths = 1;
            info.subactionPaths = &hands_[role];
            tracking_.actions.emplace_back(path.set, path.role, path.kind);
            const auto result = dispatch_.createAction(set->native, &info, &tracking_.actions.back().native);
            if (XR_FAILED(result) || tracking_.actions.back().native == XR_NULL_HANDLE)
                return Result<void>::Failure(
                    PoseActionFailure("create pose action", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::SuggestPoses */
    Result<std::uint32_t> OpenXRActionBindings::SuggestPoses(const Profile &profile, const std::span<const NativePoseActionPath> paths,
                                                             const std::span<XrActionSuggestedBinding> output) const {
        std::uint32_t count = 0;
        for (const auto &path : paths) {
            if (path.profile != profile.id)
                continue;
            const auto action = std::ranges::find_if(tracking_.actions, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind;
            });
            if (count == output.size() || action == tracking_.actions.end())
                return Result<std::uint32_t>::Failure(MakeError(XRErrors::CapacityExceeded));
            const std::string component{path.path};
            XrPath native{};
            if (const auto result = dispatch_.stringToPath(native_.instance, component.c_str(), &native);
                XR_FAILED(result) || native == XR_NULL_PATH)
                return Result<std::uint32_t>::Failure(
                    PoseActionFailure("resolve pose path", XR_FAILED(result) ? result : XR_ERROR_PATH_INVALID));
            output[count++] = {action->native, native};
        }
        return Result<std::uint32_t>::Success(count);
    }

    /** @copydoc OpenXRActionBindings::ClosePoseActions */
    Result<void> OpenXRActionBindings::ClosePoseActions() {
        for (auto action = tracking_.actions.rbegin(); action != tracking_.actions.rend(); ++action) {
            if (action->native == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroyAction(action->native);
            if (XR_FAILED(result))
                return Result<void>::Failure(PoseActionFailure("destroy pose action", result));
            action->native = XR_NULL_HANDLE;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
