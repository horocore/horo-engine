#include "host/OpenXRActionBindings.h"

#include <cmath>
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

        /** @brief Converts the admitted reference kind to its native identifier. */
        XrReferenceSpaceType NativeReference(const XRSpaceKind kind) noexcept {
            if (kind == XRSpaceKind::Stage)
                return XR_REFERENCE_SPACE_TYPE_STAGE;
            if (kind == XRSpaceKind::View)
                return XR_REFERENCE_SPACE_TYPE_VIEW;
            return XR_REFERENCE_SPACE_TYPE_LOCAL;
        }

        /** @brief Maps the only two options admitted as the application tracking reference. */
        XRSpaceKind AlternateKind(const XRSpaceKind base) noexcept {
            return base == XRSpaceKind::Stage ? XRSpaceKind::Local : XRSpaceKind::Stage;
        }

        /** @brief Preserves exact native space creation/retirement evidence. */
        Error SpaceFailure(const char *phase, const XrResult result) {
            if (result == XR_ERROR_REFERENCE_SPACE_UNSUPPORTED || result == XR_ERROR_FUNCTION_UNSUPPORTED)
                return MakeError(XRErrors::OperationUnsupported, phase);
            if (result == XR_ERROR_LIMIT_REACHED)
                return MakeError(XRErrors::CapacityExceeded, phase);
            if (result == XR_SESSION_LOSS_PENDING || result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST)
                return MakeError(XRErrors::RuntimeUnavailable, std::format("OpenXR {} interrupted by runtime loss; native result={}", phase,
                                                                           static_cast<std::int32_t>(result)));
            return MakeError(XRErrors::OperationUnavailable,
                             std::format("OpenXR {} failed; native result={}", phase, static_cast<std::int32_t>(result)));
        }

        /** @brief Only a complete creation on a live session may proceed to space publication.
         * @details Loss-pending calls can write valid handles. Their original owner retains them for teardown.
         */
        Result<void> CheckSpaceCreation(const char *phase, const XrResult result, const XrSpace space) {
            if (result != XR_SUCCESS)
                return Result<void>::Failure(SpaceFailure(phase, result));
            if (space == XR_NULL_HANDLE)
                return Result<void>::Failure(SpaceFailure(phase, XR_ERROR_RUNTIME_FAILURE));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OpenXRActionBindings::Tracking::SpaceCount */
    std::size_t OpenXRActionBindings::Tracking::SpaceCount() const noexcept {
        return actions.size() + 2 + static_cast<std::size_t>(request->worldFromBase.has_value()) +
               static_cast<std::size_t>(request->alternateReferenceAvailable) +
               static_cast<std::size_t>(request->consentedGazeSet.has_value());
    }

    /** @copydoc OpenXRActionBindings::Tracking::ReadStageBounds */
    Result<void> OpenXRActionBindings::Tracking::ReadStageBounds(const TrackingContext &context) {
        XrExtent2Df extent{};
        const auto result = context.DispatchTable().getBounds(context.Native().session, XR_REFERENCE_SPACE_TYPE_STAGE, &extent);
        if (result == XR_SPACE_BOUNDS_UNAVAILABLE && extent.width == 0.0F && extent.height == 0.0F)
            return Result<void>::Success();
        if (result != XR_SUCCESS && result != XR_SPACE_BOUNDS_UNAVAILABLE)
            return Result<void>::Failure(SpaceFailure("read Stage bounds", result));
        if (result != XR_SUCCESS || !std::isfinite(extent.width) || !std::isfinite(extent.height) || extent.width <= 0.0F ||
            extent.height <= 0.0F)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "OpenXR returned malformed Stage bounds."));
        stageBounds = XRStageBounds{extent.width, extent.height};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::Tracking::PrepareSpaces */
    Result<void> OpenXRActionBindings::Tracking::PrepareSpaces(const TrackingContext &context) {
        if (!request)
            return Result<void>::Success();
        if (!context.Preparing() && context.Native().maximumSpaces < SpaceCount())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        info.poseInReferenceSpace.orientation.w = 1.0F;  // Native space offset, never a fabricated tracked pose.
        const auto createReference = [&](const char *phase, const XRSpaceKind kind, XrSpace &space) {
            info.referenceSpaceType = NativeReference(kind);
            const auto result = context.DispatchTable().createReference(context.Native().session, &info, &space);
            return CheckSpaceCreation(phase, result, space);
        };
        if (auto created = createReference("create tracking reference space", request->base, base); created.HasError())
            return created;
        if (auto created = createReference("create head reference space", XRSpaceKind::View, view); created.HasError())
            return created;
        if (request->alternateReferenceAvailable) {
            if (auto created = createReference("create alternate reference space", AlternateKind(request->base), alternate);
                created.HasError())
                return created;
        }
        if (request->base == XRSpaceKind::Stage || alternate != XR_NULL_HANDLE)
            if (auto bounds = ReadStageBounds(context); bounds.HasError())
                return bounds;
        if (auto preparedWorld = PrepareWorldSpace(context); preparedWorld.HasError())
            return preparedWorld;
        for (auto &action : actions) {
            const std::size_t role = action.role == XRTrackedDeviceRole::LeftController ? 0 : 1;
            XrActionSpaceCreateInfo actionInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
            actionInfo.action = action.native;
            actionInfo.subactionPath = context.Hands()[role];
            actionInfo.poseInActionSpace.orientation.w = 1.0F;
            const auto result = context.DispatchTable().createSpace(context.Native().session, &actionInfo, &action.space);
            if (auto created = CheckSpaceCreation("create action space", result, action.space); created.HasError())
                return created;
        }
        return PrepareGazeSpace(context);
    }

    /** @copydoc OpenXRActionBindings::Tracking::PrepareGazeSpace */
    Result<void> OpenXRActionBindings::Tracking::PrepareGazeSpace(const TrackingContext &context) {
        if (gazeAction == XR_NULL_HANDLE)
            return Result<void>::Success();
        XrActionSpaceCreateInfo info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        info.action = gazeAction;
        info.poseInActionSpace.orientation.w = 1.0F;
        const auto result = context.DispatchTable().createSpace(context.Native().session, &info, &gazeSpace);
        return CheckSpaceCreation("create gaze space", result, gazeSpace);
    }

    /** @copydoc OpenXRActionBindings::Tracking::PrepareWorldSpace */
    Result<void> OpenXRActionBindings::Tracking::PrepareWorldSpace(const TrackingContext &context) {
        if (!request->worldFromBase)
            return Result<void>::Success();
        XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        // OpenXR expects the pose of the NEW World origin expressed in the native base.
        // Authored calibration maps base -> World, so the creation offset is its rigid inverse.
        const auto pose = CalibratedWorldPose(*request->worldFromBase);
        if (pose.HasError())
            return Result<void>::Failure(pose.ErrorValue());
        info.referenceSpaceType = NativeReference(request->base);
        info.poseInReferenceSpace = pose.Value();
        const auto result = context.DispatchTable().createReference(context.Native().session, &info, &world);
        return CheckSpaceCreation("create calibrated World reference space", result, world);
    }

    /** @copydoc OpenXRActionBindings::Tracking::BindSpaces */
    Result<void> OpenXRActionBindings::Tracking::BindSpaces(const XRSessionId &session) {
        if (!request)
            return Result<void>::Success();
        if (!session.IsValid() || base == XR_NULL_HANDLE || view == XR_NULL_HANDLE ||
            generation == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (request->worldFromBase && world == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        for (const auto &action : actions)
            if (action.space == XR_NULL_HANDLE || action.native == XR_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto issuedGeneration = ++generation;
        nextSpaceSlot = 0;
        const auto issue = [&](const XRSpaceKind kind) {
            return XRCoordinateSpace{{session, {nextSpaceSlot++, issuedGeneration}}, kind, request->origin};
        };
        baseIdentity = issue(request->base);
        viewIdentity = issue(XRSpaceKind::View);
        if (alternate != XR_NULL_HANDLE)
            alternateIdentity = issue(AlternateKind(request->base));
        for (auto &action : actions)
            action.identity = issue(XRSpaceKind::Action);
        if (world != XR_NULL_HANDLE)
            worldIdentity = issue(XRSpaceKind::World);
        if (gazeSpace != XR_NULL_HANDLE)
            gazeIdentity = issue(XRSpaceKind::Action);
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::Tracking::CloseSpaces */
    Result<void> OpenXRActionBindings::Tracking::CloseSpaces(const TrackingContext &context) {
        if (gazeSpace != XR_NULL_HANDLE) {
            const auto result = context.DispatchTable().destroySpace(gazeSpace);
            if (XR_FAILED(result))
                return Result<void>::Failure(SpaceFailure("destroy gaze space", result));
            gazeSpace = XR_NULL_HANDLE;
            gazeIdentity = {};
        }
        for (auto action = actions.rbegin(); action != actions.rend(); ++action) {
            if (action->space == XR_NULL_HANDLE)
                continue;
            const auto result = context.DispatchTable().destroySpace(action->space);
            if (XR_FAILED(result))
                return Result<void>::Failure(SpaceFailure("destroy action space", result));
            action->space = XR_NULL_HANDLE;
            action->identity = {};
        }
        for (auto *space : {&world, &alternate, &view, &base}) {
            if (*space == XR_NULL_HANDLE)
                continue;
            const auto result = context.DispatchTable().destroySpace(*space);
            if (XR_FAILED(result))
                return Result<void>::Failure(SpaceFailure("destroy tracking reference space", result));
            *space = XR_NULL_HANDLE;
        }
        baseIdentity = {};
        viewIdentity = {};
        worldIdentity = {};
        alternateIdentity = {};
        stageBounds.reset();
        eyeIdentities = {};
        nextSpaceSlot = 0;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRActionBindings::References */
    Result<XRReferenceSpaceSnapshot> OpenXRActionBindings::References(const XRSessionId &session) const {
        using Outcome = Result<XRReferenceSpaceSnapshot>;
        if (auto lane = ControlLane(); lane.HasError())
            return Outcome::Failure(lane.ErrorValue());
        if (sampling_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{sampling_};
        if (const auto current = Current(session); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!tracking_.request)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto &local = tracking_.request->base == XRSpaceKind::Local ? tracking_.baseIdentity : tracking_.alternateIdentity;
        const auto &stage = tracking_.request->base == XRSpaceKind::Stage ? tracking_.baseIdentity : tracking_.alternateIdentity;
        if (!local.id.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        return Outcome::Success(
            {session, tracking_.viewIdentity, local, stage.id.IsValid() ? std::optional{stage} : std::nullopt, tracking_.stageBounds});
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
