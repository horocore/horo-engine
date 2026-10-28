#include "host/OpenXRActionBindings.h"

#include <format>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Converts authored base-to-World calibration into the same rigid inverse native offset. */
        Result<XrPosef> CalibratedWorldPose(const Math::Transform &world) {
            const auto rotation = world.rotation.TryNormalized();
            if (rotation.HasError())
                return Result<XrPosef>::Failure(rotation.ErrorValue());
            const auto inverse = rotation.Value().Inverse();
            const auto position = inverse.Rotate(-world.translation);
            return Result<XrPosef>::Success({{inverse.x, inverse.y, inverse.z, inverse.w}, {position.x, position.y, position.z}});
        }

        /** @brief Preserves exact native space creation/retirement evidence. */
        Error SpaceFailure(const char *phase, const XrResult result) {
            return MakeError(XRErrors::OperationUnavailable,
                             std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::PrepareSpaces */
    Result<void> OpenXRActionBindings::PrepareSpaces() {
        if (!tracking_.request)
            return Result<void>::Success();
        if (!preparation_ &&
            native_.maximumSpaces < tracking_.actions.size() + 2 + (tracking_.request->worldFromBase.has_value() ? 1U : 0U))
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto reference =
            tracking_.request->base == XRSpaceKind::Stage ? XR_REFERENCE_SPACE_TYPE_STAGE : XR_REFERENCE_SPACE_TYPE_LOCAL;
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.referenceSpaceType = reference;
        info.poseInReferenceSpace.orientation.w = 1.0F;  // Native space offset, never a fabricated tracked pose.
        auto result = dispatch_.createReference(native_.session, &info, &tracking_.base);
        if (XR_FAILED(result) || tracking_.base == XR_NULL_HANDLE)
            return Result<void>::Failure(
                SpaceFailure("create tracking reference space", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        result = dispatch_.createReference(native_.session, &info, &tracking_.view);
        if (XR_FAILED(result) || tracking_.view == XR_NULL_HANDLE)
            return Result<void>::Failure(
                SpaceFailure("create head reference space", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        if (tracking_.request->worldFromBase) {
            // OpenXR expects the pose of the NEW World origin expressed in the native base.
            // Authored calibration maps base -> World, so the creation offset is its rigid inverse.
            const auto pose = CalibratedWorldPose(*tracking_.request->worldFromBase);
            if (pose.HasError())
                return Result<void>::Failure(pose.ErrorValue());
            info.referenceSpaceType = reference;
            info.poseInReferenceSpace = pose.Value();
            result = dispatch_.createReference(native_.session, &info, &tracking_.world);
            if (XR_FAILED(result) || tracking_.world == XR_NULL_HANDLE)
                return Result<void>::Failure(
                    SpaceFailure("create calibrated World reference space", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        for (auto &action : tracking_.actions) {
            const std::size_t role = action.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            XrActionSpaceCreateInfo actionInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            actionInfo.action = action.native;
            actionInfo.subactionPath = hands_[role];
            actionInfo.poseInActionSpace.orientation.w = 1.0F;
            result = dispatch_.createSpace(native_.session, &actionInfo, &action.space);
            if (XR_FAILED(result) || action.space == XR_NULL_HANDLE)
                return Result<void>::Failure(SpaceFailure("create action space", XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::BindSpaces */
    Result<void> OpenXRActionBindings::BindSpaces(const XRSessionId &session) {
        if (!tracking_.request)
            return Result<void>::Success();
        if (!session.IsValid() || tracking_.base == XR_NULL_HANDLE || tracking_.view == XR_NULL_HANDLE ||
            tracking_.generation == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (tracking_.request->worldFromBase && tracking_.world == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        for (const auto &action : tracking_.actions)
            if (action.space == XR_NULL_HANDLE || action.native == XR_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto generation = ++tracking_.generation;
        tracking_.nextSpaceSlot = 0;
        const auto issue = [&](const XRSpaceKind kind) {
            return XRCoordinateSpace{{session, {tracking_.nextSpaceSlot++, generation}}, kind, tracking_.request->origin};
        };
        tracking_.baseIdentity = issue(tracking_.request->base);
        tracking_.viewIdentity = issue(XRSpaceKind::View);
        for (auto &action : tracking_.actions)
            action.identity = issue(XRSpaceKind::Action);
        if (tracking_.world != XR_NULL_HANDLE)
            tracking_.worldIdentity = issue(XRSpaceKind::World);
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::CloseSpaces */
    Result<void> OpenXRActionBindings::CloseSpaces() {
        for (auto action = tracking_.actions.rbegin(); action != tracking_.actions.rend(); ++action) {
            if (action->space == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroySpace(action->space);
            if (XR_FAILED(result))
                return Result<void>::Failure(SpaceFailure("destroy action space", result));
            action->space = XR_NULL_HANDLE;
            action->identity = {};
        }
        for (auto *space : {&tracking_.world, &tracking_.view, &tracking_.base}) {
            if (*space == XR_NULL_HANDLE)
                continue;
            const auto result = dispatch_.destroySpace(*space);
            if (XR_FAILED(result))
                return Result<void>::Failure(SpaceFailure("destroy tracking reference space", result));
            *space = XR_NULL_HANDLE;
        }
        tracking_.baseIdentity = {};
        tracking_.viewIdentity = {};
        tracking_.worldIdentity = {};
        tracking_.eyeIdentities = {};
        tracking_.nextSpaceSlot = 0;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::WorldSpace */
    Result<XRCoordinateSpace> OpenXRActionBindings::WorldSpace(const XRSessionId &session) const {
        if (auto lane = ControlLane(); lane.HasError())
            return Result<XRCoordinateSpace>::Failure(lane.ErrorValue());
        if (sampling_)
            return Result<XRCoordinateSpace>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (const auto current = Current(session); current.HasError())
            return Result<XRCoordinateSpace>::Failure(current.ErrorValue());
        if (tracking_.world == XR_NULL_HANDLE || !tracking_.worldIdentity.id.IsValid())
            return Result<XRCoordinateSpace>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<XRCoordinateSpace>::Success(tracking_.worldIdentity);
    }

    /** @copydoc OpenXRActionBindings::BorrowWorldSpace */
    Result<XrSpace> OpenXRActionBindings::BorrowWorldSpace(const XRCoordinateSpace &space) const {
        if (auto lane = ControlLane(); lane.HasError())
            return Result<XrSpace>::Failure(lane.ErrorValue());
        if (sampling_)
            return Result<XrSpace>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (const auto current = Current(space.id.session); current.HasError())
            return Result<XrSpace>::Failure(current.ErrorValue());
        if (tracking_.world == XR_NULL_HANDLE || space != tracking_.worldIdentity)
            return Result<XrSpace>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<XrSpace>::Success(tracking_.world);
    }

    /** @copydoc OpenXRActionBindings::BorrowPresentationReference */
    Result<XrSpace> OpenXRActionBindings::BorrowPresentationReference(const XRCoordinateSpace &space) const {
        if (auto lane = ControlLane(); lane.HasError())
            return Result<XrSpace>::Failure(lane.ErrorValue());
        if (sampling_)
            return Result<XrSpace>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (const auto current = Current(space.id.session); current.HasError())
            return Result<XrSpace>::Failure(current.ErrorValue());
        if (tracking_.base == XR_NULL_HANDLE || space != tracking_.baseIdentity)
            return Result<XrSpace>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<XrSpace>::Success(tracking_.base);
    }
}  // namespace Horo::XR::OpenXRInternal
