#include "host/OpenXRHandTracking.h"

#include "Horo/Foundation/Assertions.h"

#include <bit>
#include <cmath>
#include <format>

namespace Horo::XR::OpenXRInternal {
    namespace {
        static_assert(XRHandTrackingHardLimits::JointsPerHand == XR_HAND_JOINT_COUNT_EXT);
        static_assert(static_cast<std::uint32_t>(XRHandJoint::Palm) == XR_HAND_JOINT_PALM_EXT);
        static_assert(static_cast<std::uint32_t>(XRHandJoint::LittleTip) == XR_HAND_JOINT_LITTLE_TIP_EXT);
        constexpr std::array nativeHands{XR_HAND_LEFT_EXT, XR_HAND_RIGHT_EXT};
        constexpr NativeFeatureProviderRoute HandSamplingRoute{XR_EXT_HAND_TRACKING_EXTENSION_NAME, "xrLocateHandJointsEXT"};

        /** @brief Keeps a same-lane foreign operation from recursively mutating the tracker pair. */
        class Operation final {
        public:
            explicit Operation(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~Operation() {
                active_ = false;
            }

            Operation(const Operation &) = delete;
            Operation &operator=(const Operation &) = delete;

        private:
            bool &active_;
        };

        /** @brief Translates native hand dispatch failures at the concrete backend boundary. */
        Error HandError(const char *function, const XrResult result) {
            const auto &descriptor =
                result == XR_ERROR_PERMISSION_INSUFFICIENT ? XRErrors::PermissionDenied : XRErrors::OperationUnavailable;
            return MakeError(descriptor, std::format("{} failed; native result={}", function, static_cast<std::int32_t>(result)));
        }

        /** @brief Distinguishes optional hand-only failures from loss of shared session or clock evidence. */
        Error HandSampleError(const XrResult result) {
            const auto &descriptor = [result]() -> const ErrorCodeDescriptor & {
                switch (result) {
                    case XR_ERROR_PERMISSION_INSUFFICIENT:
                        return XRErrors::PermissionDenied;
                    case XR_ERROR_FUNCTION_UNSUPPORTED:
                        return XRErrors::OperationUnsupported;
                    case XR_ERROR_HANDLE_INVALID:
                        return XRErrors::IdentityStale;
                    case XR_ERROR_TIME_INVALID:
                        return XRErrors::TimeDomainIncompatible;
                    case XR_ERROR_VALIDATION_FAILURE:
                        return XRErrors::OperationInvalid;
                    default:
                        return XRErrors::RuntimeUnavailable;
                }
            }();
            return MakeError(descriptor, std::format("xrLocateHandJointsEXT failed (XrResult {})", static_cast<std::int32_t>(result)));
        }

        /** @brief Resolves one required tracker lifecycle dispatch from the selected instance. */
        template <typename Function> Result<Function> Resolve(const NativeDiscoveryBorrow &discovery, const char *name) {
            PFN_xrVoidFunction function{};
            const auto result = discovery.getProc(discovery.instance, name, &function);
            if (result != XR_SUCCESS || !function)
                return Result<Function>::Failure(HandError(name, result));
            return Result<Function>::Success(std::bit_cast<Function>(function));
        }

        /** @brief Maps independent valid/tracked evidence without reading undefined native components. */
        Result<NativeHandJointSample> Normalize(const XrHandJointLocationEXT &location, const XRCoordinateSpace &space,
                                                const XRTimeEvidence &time) {
            using Outcome = Result<NativeHandJointSample>;
            using enum XRPoseComponentValidity;
            const auto flags = location.locationFlags;
            constexpr auto position = XR_SPACE_LOCATION_POSITION_VALID_BIT;
            constexpr auto orientation = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            constexpr auto trackedPosition = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
            constexpr auto trackedOrientation = XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            if ((flags & ~(position | orientation | trackedPosition | trackedOrientation)) != 0 ||
                ((flags & trackedPosition) && !(flags & position)) || ((flags & trackedOrientation) && !(flags & orientation)))
                return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
            if (!(flags & (position | orientation)))
                return Outcome::Success({});
            XRPoseComponents components;
            std::optional<float> radius;
            if (flags & position) {
                if (!std::isfinite(location.radius) || location.radius <= 0.0F)
                    return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
                radius = location.radius;
                components.positionMeters = {Math::Vec3{location.pose.position.x, location.pose.position.y, location.pose.position.z},
                                             flags & trackedPosition ? Tracked : Inferred};
            }
            if (flags & orientation)
                components.orientation = {Math::Quaternion{location.pose.orientation.x, location.pose.orientation.y,
                                                           location.pose.orientation.z, location.pose.orientation.w},
                                          flags & trackedOrientation ? Tracked : Inferred};
            components.confidence =
                (flags & trackedPosition) && (flags & trackedOrientation) ? XRTrackingConfidence::High : XRTrackingConfidence::Low;
            components.loss = !(flags & position)      ? XRTrackingLossState::PositionLost
                              : !(flags & orientation) ? XRTrackingLossState::OrientationLost
                                                       : XRTrackingLossState::None;
            auto sample = XRPoseSample::Create({space.id.session, space, space, XRPosePurpose::SimulationInput, time, components},
                                               space.id.session, space.worldOriginRevision);
            if (sample.HasError())
                return Outcome::Failure(sample.ErrorValue());
            return Outcome::Success({std::move(sample).Value(), radius});
        }

        /** @brief Checks fixed native result shape before any joint can escape. */
        bool ValidLocations(const XrHandJointLocationsEXT &locations, std::span<const XrHandJointLocationEXT> storage) noexcept {
            return locations.type == XR_TYPE_HAND_JOINT_LOCATIONS_EXT && !locations.next && locations.isActive <= XR_TRUE &&
                   locations.jointCount == storage.size() && locations.jointLocations == storage.data();
        }
    }  // namespace

    OpenXRHandTracking::~OpenXRHandTracking() {
        HORO_INVARIANT_MSG(!destroy_, "Close native hand trackers before destroying their owner");
    }

    /** @copydoc OpenXRHandTracking::Prepare */
    Result<void> OpenXRHandTracking::Prepare(const NativeSessionPreparation &preparation) {
        if (std::this_thread::get_id() != lane_ || active_ || destroy_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{active_};
        const auto admission = native_->ReadExtensionAdmission(preparation.activeAttempt, HandSamplingRoute.extension);
        if (admission.HasError())
            return Result<void>::Failure(admission.ErrorValue());
        if (admission.Value().state != NativeExtensionState::Enabled)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto discovery = native_->BorrowDiscovery(preparation.activeAttempt);
        if (discovery.HasError())
            return Result<void>::Failure(discovery.ErrorValue());
        const auto prepared = native_->BorrowPreparedSession(preparation);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        const auto create = Resolve<PFN_xrCreateHandTrackerEXT>(discovery.Value(), "xrCreateHandTrackerEXT");
        if (create.HasError())
            return Result<void>::Failure(create.ErrorValue());
        const auto destroy = Resolve<PFN_xrDestroyHandTrackerEXT>(discovery.Value(), "xrDestroyHandTrackerEXT");
        if (destroy.HasError())
            return Result<void>::Failure(destroy.ErrorValue());
        const auto locate = Resolve<PFN_xrLocateHandJointsEXT>(discovery.Value(), HandSamplingRoute.extensionSymbol.data());
        if (locate.HasError())
            return Result<void>::Failure(locate.ErrorValue());
        auto current = native_->BorrowPreparedSession(preparation);
        if (current.HasError() || current.Value().incarnation != prepared.Value().incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        destroy_ = destroy.Value();
        incarnation_ = prepared.Value().incarnation;
        for (std::size_t hand = 0; hand < trackers_.size(); ++hand) {
            const XrHandTrackerCreateInfoEXT info{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, nullptr, nativeHands[hand],
                                                  XR_HAND_JOINT_SET_DEFAULT_EXT};
            XrHandTrackerEXT tracker{XR_NULL_HANDLE};
            const auto result = create.Value()(current.Value().session, &info, &tracker);
            if (XR_SUCCEEDED(result))
                trackers_[hand] = tracker;
            if (result != XR_SUCCESS)
                return Result<void>::Failure(HandError("xrCreateHandTrackerEXT", result));
            if (tracker == XR_NULL_HANDLE)
                return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Hand tracker creation returned an empty success"));

            current = native_->BorrowPreparedSession(preparation);
            if (current.HasError() || current.Value().incarnation != incarnation_)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        }
        preparedLocate_ = locate.Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRHandTracking::Bind */
    Result<void> OpenXRHandTracking::Bind(const XRSessionId &session) {
        if (std::this_thread::get_id() != lane_ || active_ || provider_ || !preparedLocate_ || !trackers_.front() || !trackers_.back())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{active_};
        const OpenXRNativeSession::PreparedFeatureDispatch prepared{incarnation_, std::bit_cast<PFN_xrVoidFunction>(preparedLocate_)};
        auto provider = native_->RegisterFeatureProvider(session, HandSamplingRoute, prepared);
        if (provider.HasError())
            return Result<void>::Failure(provider.ErrorValue());
        provider_.emplace(std::move(provider).Value());
        preparedLocate_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRHandTracking::Sample */
    Result<NativeHandSamples> OpenXRHandTracking::Sample(XrSpace nativeSpace, const XRCoordinateSpace &space,
                                                         const XRTimeEvidence &time) const {
        using Outcome = Result<NativeHandSamples>;
        if (std::this_thread::get_id() != lane_ || active_ || !provider_ || nativeSpace == XR_NULL_HANDLE || !time.runtimeSample.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{active_};
        auto provider = native_->BorrowFeatureProvider(*provider_);
        if (provider.HasError())
            return Outcome::Failure(provider.ErrorValue());
        if (space.id.session != native_->Session())
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto locate = std::bit_cast<PFN_xrLocateHandJointsEXT>(provider.Value().function);
        const XrHandJointsLocateInfoEXT info{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, nativeSpace, time.runtimeSample.Nanoseconds()};
        NativeHandSamples output;
        for (std::size_t hand = 0; hand < trackers_.size(); ++hand) {
            std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> nativeJoints{};
            XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE,
                                              static_cast<std::uint32_t>(nativeJoints.size()), nativeJoints.data()};
            const auto result = locate(trackers_[hand], &info, &locations);
            provider = native_->BorrowFeatureProvider(*provider_);
            if (provider.HasError())
                return Outcome::Failure(provider.ErrorValue());
            if (result != XR_SUCCESS)
                return Outcome::Failure(HandSampleError(result));
            if (!ValidLocations(locations, nativeJoints))
                return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            if (locations.isActive != XR_TRUE)
                continue;
            for (std::size_t joint = 0; joint < nativeJoints.size(); ++joint) {
                auto normalized = Normalize(nativeJoints[joint], space, time);
                if (normalized.HasError())
                    return Outcome::Failure(normalized.ErrorValue());
                output.hands[hand].joints[joint] = std::move(normalized).Value();
            }
        }
        return Outcome::Success(std::move(output));
    }

    /** @copydoc OpenXRHandTracking::Close */
    Result<void> OpenXRHandTracking::Close() {
        if (std::this_thread::get_id() != lane_ || active_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const Operation operation{active_};
        if (provider_) {
            native_->RevokeFeatureProvider(*provider_);
            provider_.reset();
        }
        for (auto it = trackers_.rbegin(); it != trackers_.rend(); ++it) {
            if (*it == XR_NULL_HANDLE)
                continue;
            const auto result = destroy_(*it);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(HandError("xrDestroyHandTrackerEXT", result));
            *it = XR_NULL_HANDLE;
        }
        destroy_ = {};
        preparedLocate_ = {};
        incarnation_ = {};
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
