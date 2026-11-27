#include "Horo/XR/XRSpacePose.h"

#include "Horo/XR/XRErrors.h"

#include <cmath>
#include <utility>

namespace Horo::XR {
    namespace {
        /** @brief Creates a typed failed result from a stable XR descriptor. */
        template <typename T> [[nodiscard]] Result<T> Reject(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        /** @brief Validates a semantic space without constructing heavyweight failure state on success. */
        [[nodiscard]] Result<void> ValidateSpace(const XRCoordinateSpace &space, const XRSessionId &activeSession,
                                                 const XRWorldOriginRevision activeOriginRevision) {
            if (auto identity = ValidateXRSessionObject(space.id, activeSession); identity.HasError())
                return identity;
            if (space.kind >= XRSpaceKind::Count || space.convention >= XRCoordinateConvention::Count)
                return Reject<void>(XRErrors::OperationInvalid);
            if (space.convention != XRCoordinateConvention::RightHandedYUpNegativeZForward)
                return Reject<void>(XRErrors::OperationUnsupported);
            if (!space.worldOriginRevision.IsValid() || !activeOriginRevision.IsValid())
                return Reject<void>(XRErrors::OperationInvalid);
            if (space.worldOriginRevision != activeOriginRevision)
                return Reject<void>(XRErrors::OriginRevisionStale);
            return Result<void>::Success();
        }

        /** @brief Validates the presence and representation of one independently tagged component. */
        template <typename Value, typename IsValid>
        [[nodiscard]] bool ValidComponent(const XRPoseComponent<Value> &component, IsValid &&isValid) {
            if (component.validity >= XRPoseComponentValidity::Count)
                return false;
            if (component.validity == XRPoseComponentValidity::Invalid)
                return !component.value.has_value();
            return component.value.has_value() && isValid(*component.value);
        }

        /** @brief Checks that an orientation is finite and normalized within the contract tolerance. */
        [[nodiscard]] bool UnitQuaternion(const Math::Quaternion value) noexcept {
            if (!Math::IsFinite(value))
                return false;
            const float lengthSquared = value.x * value.x + value.y * value.y + value.z * value.z + value.w * value.w;
            return std::abs(lengthSquared - 1.0F) <= 0.0001F;
        }

        /** @brief Checks that exactly the time domain required by the pose purpose is present. */
        [[nodiscard]] bool ValidTime(const XRPoseDescriptor &descriptor) noexcept {
            if (!descriptor.time.runtimeSample.IsValid())
                return false;
            using enum XRPosePurpose;
            switch (descriptor.purpose) {
                case SimulationInput:
                    return descriptor.time.simulation.has_value() && descriptor.time.simulation->IsValid() &&
                           !descriptor.time.renderPrediction.has_value();
                case PresentationPrediction:
                    return !descriptor.time.simulation.has_value() && descriptor.time.renderPrediction.has_value() &&
                           descriptor.time.renderPrediction->IsValid();
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Checks aggregate tracking loss against independently available components. */
        [[nodiscard]] bool ValidLoss(const XRPoseComponents &components) noexcept {
            const bool positionPresent = components.positionMeters.validity != XRPoseComponentValidity::Invalid;
            const bool orientationPresent = components.orientation.validity != XRPoseComponentValidity::Invalid;
            const bool linearVelocityPresent = components.linearVelocityMetersPerSecond.validity != XRPoseComponentValidity::Invalid;
            const bool angularVelocityPresent = components.angularVelocityRadiansPerSecond.validity != XRPoseComponentValidity::Invalid;
            using enum XRTrackingLossState;
            switch (components.loss) {
                case None:
                    return positionPresent && orientationPresent && components.confidence != XRTrackingConfidence::None;
                case PositionLost:
                    return !positionPresent && !linearVelocityPresent && orientationPresent &&
                           components.confidence != XRTrackingConfidence::None;
                case OrientationLost:
                    return positionPresent && !orientationPresent && !angularVelocityPresent &&
                           components.confidence != XRTrackingConfidence::None;
                case FullyLost:
                    return !positionPresent && !orientationPresent && !linearVelocityPresent && !angularVelocityPresent &&
                           components.confidence == XRTrackingConfidence::None;
                case Count:
                    return false;
            }
            return false;
        }

        /** @brief Validates every pose component and its aggregate confidence and loss evidence. */
        [[nodiscard]] bool ValidComponents(const XRPoseComponents &components) {
            if (components.confidence >= XRTrackingConfidence::Count)
                return false;
            const auto finiteVector = [](const Math::Vec3 value) {
                return Math::IsFinite(value);
            };
            return ValidComponent(components.positionMeters, finiteVector) && ValidComponent(components.orientation, UnitQuaternion) &&
                   ValidComponent(components.linearVelocityMetersPerSecond, finiteVector) &&
                   ValidComponent(components.angularVelocityRadiansPerSecond, finiteVector) && ValidLoss(components);
        }

        /** @brief Conservatively combines two component-validity provenances. */
        [[nodiscard]] XRPoseComponentValidity CombinedValidity(const XRPoseComponentValidity first,
                                                               const XRPoseComponentValidity second) noexcept {
            using enum XRPoseComponentValidity;
            if (first == Invalid || second == Invalid)
                return Invalid;
            return first == Tracked && second == Tracked ? Tracked : Inferred;
        }

        /** @brief Conservatively combines two tracking-confidence categories. */
        [[nodiscard]] XRTrackingConfidence CombinedConfidence(const XRTrackingConfidence first,
                                                              const XRTrackingConfidence second) noexcept {
            using enum XRTrackingConfidence;
            if (first == None || second == None)
                return None;
            return first == Low || second == Low ? Low : High;
        }

        /** @brief Combines tracking-loss categories without restoring missing evidence. */
        [[nodiscard]] XRTrackingLossState CombinedLoss(const XRTrackingLossState first, const XRTrackingLossState second) noexcept {
            using enum XRTrackingLossState;
            if (first == FullyLost || second == FullyLost)
                return FullyLost;
            if (first == None)
                return second;
            if (second == None || first == second)
                return first;
            return FullyLost;
        }

        /** @brief Rotates and adds two vector components while retaining weakest validity. */
        [[nodiscard]] XRPoseComponent<Math::Vec3> ComposeVector(const XRPoseComponent<Math::Vec3> &first,
                                                                const XRPoseComponent<Math::Vec3> &second,
                                                                const XRPoseComponent<Math::Quaternion> &rotation) {
            const auto validity = CombinedValidity(CombinedValidity(first.validity, second.validity), rotation.validity);
            if (validity == XRPoseComponentValidity::Invalid)
                return {};
            return {.value = rotation.value->Rotate(*first.value) + *second.value, .validity = validity};
        }

        /** @brief Composes linear velocities including moving-reference angular motion. */
        [[nodiscard]] XRPoseComponent<Math::Vec3> ComposeLinearVelocity(const XRPoseComponents &first, const XRPoseComponents &second) {
            auto validity = CombinedValidity(first.linearVelocityMetersPerSecond.validity, second.linearVelocityMetersPerSecond.validity);
            validity = CombinedValidity(validity, second.orientation.validity);
            validity = CombinedValidity(validity, second.angularVelocityRadiansPerSecond.validity);
            validity = CombinedValidity(validity, first.positionMeters.validity);
            if (validity == XRPoseComponentValidity::Invalid)
                return {};

            const Math::Vec3 rotatedPosition = second.orientation.value->Rotate(*first.positionMeters.value);
            const Math::Vec3 referenceMotion = Math::Cross(*second.angularVelocityRadiansPerSecond.value, rotatedPosition);
            return {.value = second.orientation.value->Rotate(*first.linearVelocityMetersPerSecond.value) +
                             *second.linearVelocityMetersPerSecond.value + referenceMotion,
                    .validity = validity};
        }

        /** @brief Composes two validated component sets without allocating or fabricating values. */
        [[nodiscard]] XRPoseComponents ComposeComponents(const XRPoseComponents &first, const XRPoseComponents &second) {
            XRPoseComponents composed;
            composed.positionMeters = ComposeVector(first.positionMeters, second.positionMeters, second.orientation);
            if (const auto orientationValidity = CombinedValidity(first.orientation.validity, second.orientation.validity);
                orientationValidity != XRPoseComponentValidity::Invalid) {
                composed.orientation = {.value = *second.orientation.value * *first.orientation.value, .validity = orientationValidity};
            }
            composed.linearVelocityMetersPerSecond = ComposeLinearVelocity(first, second);
            composed.angularVelocityRadiansPerSecond =
                ComposeVector(first.angularVelocityRadiansPerSecond, second.angularVelocityRadiansPerSecond, second.orientation);
            composed.confidence = CombinedConfidence(first.confidence, second.confidence);
            composed.loss = CombinedLoss(first.loss, second.loss);
            return composed;
        }

        /** @brief Composes one contiguous pair and revalidates the produced ownership evidence. */
        [[nodiscard]] Result<XRPoseSample> ComposePair(const XRPoseSample &first, const XRPoseSample &second,
                                                       const XRSessionId &activeSession, const XRWorldOriginRevision activeOriginRevision) {
            if (first.Target() != second.Source())
                return Reject<XRPoseSample>(XRErrors::CoordinateSpaceIncompatible);
            if (first.Purpose() != second.Purpose() || first.Time() != second.Time())
                return Reject<XRPoseSample>(XRErrors::TimeDomainIncompatible);
            return XRPoseSample::Create({.session = first.Session(),
                                         .source = first.Source(),
                                         .target = second.Target(),
                                         .purpose = first.Purpose(),
                                         .time = first.Time(),
                                         .components = ComposeComponents(first.Components(), second.Components())},
                                        activeSession, activeOriginRevision);
        }

        /** @brief Revalidates an immutable sample against current replacement and shutdown fences. */
        [[nodiscard]] Result<XRPoseSample> Revalidate(const XRPoseSample &sample, const XRSessionId &activeSession,
                                                      const XRWorldOriginRevision activeOriginRevision) {
            return XRPoseSample::Create({.session = sample.Session(),
                                         .source = sample.Source(),
                                         .target = sample.Target(),
                                         .purpose = sample.Purpose(),
                                         .time = sample.Time(),
                                         .components = sample.Components()},
                                        activeSession, activeOriginRevision);
        }
    }  // namespace

    /** @copydoc XRPoseSample::Create */
    Result<XRPoseSample> XRPoseSample::Create(const XRPoseDescriptor &descriptor, const XRSessionId &activeSession,
                                              const XRWorldOriginRevision activeOriginRevision) {
        if (auto session = ValidateXRSession(descriptor.session, activeSession); session.HasError())
            return Result<XRPoseSample>::Failure(session.ErrorValue());
        if (auto source = ValidateSpace(descriptor.source, activeSession, activeOriginRevision); source.HasError())
            return Result<XRPoseSample>::Failure(source.ErrorValue());
        if (auto target = ValidateSpace(descriptor.target, activeSession, activeOriginRevision); target.HasError())
            return Result<XRPoseSample>::Failure(target.ErrorValue());
        if (descriptor.source.id == descriptor.target.id && descriptor.source != descriptor.target)
            return Reject<XRPoseSample>(XRErrors::CoordinateSpaceIncompatible);
        if (!ValidTime(descriptor))
            return Reject<XRPoseSample>(XRErrors::TimeDomainIncompatible);
        if (!ValidComponents(descriptor.components))
            return Reject<XRPoseSample>(XRErrors::PoseInvalid);
        return Result<XRPoseSample>::Success(XRPoseSample{descriptor});
    }

    XRPoseSample::XRPoseSample(const XRPoseDescriptor &descriptor) noexcept
        : session_(descriptor.session), source_(descriptor.source), target_(descriptor.target), purpose_(descriptor.purpose),
          time_(descriptor.time), components_(descriptor.components) {}

    /** @copydoc XRPoseSample::Session */
    const XRSessionId &XRPoseSample::Session() const noexcept {
        return session_;
    }

    /** @copydoc XRPoseSample::Source */
    const XRCoordinateSpace &XRPoseSample::Source() const noexcept {
        return source_;
    }

    /** @copydoc XRPoseSample::Target */
    const XRCoordinateSpace &XRPoseSample::Target() const noexcept {
        return target_;
    }

    /** @copydoc XRPoseSample::Purpose */
    XRPosePurpose XRPoseSample::Purpose() const noexcept {
        return purpose_;
    }

    /** @copydoc XRPoseSample::Time */
    const XRTimeEvidence &XRPoseSample::Time() const noexcept {
        return time_;
    }

    /** @copydoc XRPoseSample::Components */
    const XRPoseComponents &XRPoseSample::Components() const noexcept {
        return components_;
    }

    /** @copydoc PlaceXRWorldPose */
    Result<XRPoseSample> PlaceXRWorldPose(const XRPoseDescriptor &pose, const Math::Transform &placement) {
        if (pose.target.kind != XRSpaceKind::World)
            return Reject<XRPoseSample>(XRErrors::CoordinateSpaceIncompatible);
        const auto origin = pose.target.worldOriginRevision;
        const auto validated = XRPoseSample::Create(pose, pose.session, origin);
        if (validated.HasError())
            return validated;
        if (placement.scale != Math::Vec3{1.0F, 1.0F, 1.0F} || !Math::IsFinite(placement.translation))
            return Reject<XRPoseSample>(XRErrors::PoseInvalid);
        const auto rotation = placement.rotation.TryNormalized();
        if (rotation.HasError())
            return Reject<XRPoseSample>(XRErrors::PoseInvalid);

        auto result = pose;
        auto &components = result.components;
        const auto rotate = [&](auto &vector) {
            if (vector.value)
                vector.value = rotation.Value().Rotate(*vector.value);
        };
        rotate(components.positionMeters);
        rotate(components.linearVelocityMetersPerSecond);
        rotate(components.angularVelocityRadiansPerSecond);
        if (auto &position = components.positionMeters.value)
            position = *position + placement.translation;
        if (auto &orientation = components.orientation.value)
            orientation = rotation.Value() * *orientation;
        return XRPoseSample::Create(result, result.session, origin);
    }

    /** @copydoc ComposeXRSpaceTransforms */
    Result<XRPoseSample> ComposeXRSpaceTransforms(const std::span<const XRPoseSample> transforms, const XRSessionId &activeSession,
                                                  const XRWorldOriginRevision activeOriginRevision) {
        if (transforms.empty())
            return Reject<XRPoseSample>(XRErrors::OperationInvalid);
        if (transforms.size() > XRCoordinateHardLimits::MaximumTransformHops)
            return Reject<XRPoseSample>(XRErrors::CapacityExceeded);
        if (transforms.size() == 1)
            return Revalidate(transforms.front(), activeSession, activeOriginRevision);

        XRPoseSample composed = transforms.front();
        for (std::size_t index = 1; index < transforms.size(); ++index) {
            if (auto next = ComposePair(composed, transforms[index], activeSession, activeOriginRevision); next.HasError()) {
                return next;
            } else {
                composed = std::move(next).Value();
            }
        }
        return Result<XRPoseSample>::Success(std::move(composed));
    }
}  // namespace Horo::XR
