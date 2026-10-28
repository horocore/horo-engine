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

    /** @copydoc OpenXRActionBindings::Tracking::ValidatePosePaths */
    Result<void> OpenXRActionBindings::Tracking::ValidatePosePaths(const NativeActionBindingRequest &bindingRequest,
                                                                   const std::uint32_t maximumActions) const {
        if (!bindingRequest.tracking && bindingRequest.posePaths.empty())
            return Result<void>::Success();
        if (!bindingRequest.tracking || !bindingRequest.tracking->origin.IsValid() ||
            (bindingRequest.tracking->base != XRSpaceKind::Local && bindingRequest.tracking->base != XRSpaceKind::Stage) ||
            bindingRequest.posePaths.size() > XRActionBindingLimits::MaximumProfiles * 4)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (bindingRequest.tracking->worldFromBase) {
            const auto &world = *bindingRequest.tracking->worldFromBase;
            const auto rotation = world.rotation.TryNormalized();
            if (!Math::IsFinite(world.translation) || !Math::IsFinite(world.scale) || std::abs(world.scale.x - 1.0F) > 0.00001F ||
                std::abs(world.scale.y - 1.0F) > 0.00001F || std::abs(world.scale.z - 1.0F) > 0.00001F || rotation.HasError())
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            if (rotation.Value().Inverse().TryRotate(-world.translation).HasError())
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        std::size_t unique = 0;
        for (std::size_t index = 0; index < bindingRequest.posePaths.size(); ++index) {
            const auto &path = bindingRequest.posePaths[index];
            const auto previous = bindingRequest.posePaths.first(index);
            if (!PosePath(path) || path.label.empty() || path.label.size() >= XR_MAX_LOCALIZED_ACTION_NAME_SIZE ||
                path.label.find('\0') != std::string_view::npos ||
                std::ranges::find(bindingRequest.profiles, path.profile, &NativeInteractionProfilePath::profile) ==
                    bindingRequest.profiles.end() ||
                std::ranges::find(bindingRequest.schema.actions, path.set, &XRActionDeclaration::set) ==
                    bindingRequest.schema.actions.end() ||
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
        if (unique > 4 || bindingRequest.schema.actions.size() + unique > maximumActions)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::Tracking::CreatePoseActions */
    Result<void> OpenXRActionBindings::Tracking::CreatePoseActions(const TrackingContext &context,
                                                                   const std::span<const NativePoseActionPath> paths) {
        for (const auto &path : paths) {
            if (std::ranges::any_of(actions, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind;
            }))
                continue;
            const auto set = std::ranges::find(context.Sets(), path.set, &Set::id);
            const std::size_t role = path.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
            const auto name = std::format("horo_pose_{}", actions.size());
            std::ranges::copy(name, info.actionName);
            std::ranges::copy(path.label, info.localizedActionName);
            info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            info.countSubactionPaths = 1;
            info.subactionPaths = &context.Hands()[role];
            actions.emplace_back(path.set, path.role, path.kind);
            const auto result = context.DispatchTable().createAction(set->native, &info, &actions.back().native);
            if (XR_FAILED(result) || actions.back().native == XR_NULL_HANDLE)
                return Result<void>::Failure(
                    PoseActionFailure("create pose action", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::Tracking::SuggestPoses */
    Result<std::uint32_t> OpenXRActionBindings::Tracking::SuggestPoses(const TrackingContext &context, const Profile &profile,
                                                                       const std::span<const NativePoseActionPath> paths,
                                                                       const std::span<XrActionSuggestedBinding> output) const {
        std::uint32_t count = 0;
        for (const auto &path : paths) {
            if (path.profile != profile.id)
                continue;
            const auto action = std::ranges::find_if(actions, [&](const auto &entry) {
                return entry.role == path.role && entry.kind == path.kind;
            });
            if (count == output.size() || action == actions.end())
                return Result<std::uint32_t>::Failure(MakeError(XRErrors::CapacityExceeded));
            const std::string component{path.path};
            XrPath native{};
            if (const auto result = context.DispatchTable().stringToPath(context.Native().instance, component.c_str(), &native);
                XR_FAILED(result) || native == XR_NULL_PATH)
                return Result<std::uint32_t>::Failure(
                    PoseActionFailure("resolve pose path", XR_FAILED(result) ? result : XR_ERROR_PATH_INVALID));
            output[count++] = {action->native, native};
        }
        return Result<std::uint32_t>::Success(count);
    }

    /** @copydoc OpenXRActionBindings::Tracking::ClosePoseActions */
    Result<void> OpenXRActionBindings::Tracking::ClosePoseActions(const TrackingContext &context) {
        for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
            if (action->native == XR_NULL_HANDLE)
                continue;
            const auto result = context.DispatchTable().destroyAction(action->native);
            if (XR_FAILED(result))
                return Result<void>::Failure(PoseActionFailure("destroy pose action", result));
            action->native = XR_NULL_HANDLE;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
