#pragma once

#include "CharacterWorldPlatformInternal.h"

namespace Horo::Character::Detail {
    /** @brief Owned stage-3 motion candidate; never retains query or Physics storage. */
    struct CharacterPlatformCarry final {
        Math::Vec3 displacement{};
        Math::Quaternion heading{Math::Quaternion::Identity()};
        Math::Vec3 leaveVelocity{};
        Math::Vec3 startLeaveVelocity{};
        std::optional<CharacterPlatformBodyEvidence> predicted;
        bool stale{};
    };

    /** @brief Validates the complete bounded host target inventory before consulting a support body. */
    [[nodiscard]] Result<void> ValidatePlatformTargets(const auto &impl, const CharacterFixedTickInput &input) {
        if (input.platformTargets.size() > impl.settings.Values().work.maximumCommandsPerTick)
            return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));
        for (std::size_t index = 0; index < input.platformTargets.size(); ++index) {
            const auto &target = input.platformTargets[index];
            if (const auto valid = Physics::ValidatePhysicsKinematicTargetCommand(target, impl.descriptor.physicsWorld,
                                                                                  impl.descriptor.sceneGeneration, input.tick);
                valid.HasError())
                return valid;
            for (const auto &previous : input.platformTargets.first(index))
                if (previous.identity.body == target.identity.body)
                    return Result<void>::Failure(MakeError(CharacterErrors::RequestInvalid));
        }
        return Result<void>::Success();
    }

    /** @brief Predicts dynamic motion for one fixed quantum or selects the exact admitted kinematic target. */
    [[nodiscard]] Physics::PhysicsPose PlatformTarget(const CharacterPlatformBodyEvidence &body, const CharacterFixedTickInput &input,
                                                      const float seconds) {
        auto pose = body.pose;
        if (body.motion == Physics::PhysicsMotionType::Kinematic) {
            const auto target = std::ranges::find(input.platformTargets, body.body, [](const auto &value) {
                return value.identity.body;
            });
            if (target != input.platformTargets.end())
                pose = target->targetPose;
        } else if (body.motion == Physics::PhysicsMotionType::Dynamic) {
            pose.translation += body.linearVelocityMetersPerSecond * seconds;
            const float speed = Math::Length(body.angularVelocityRadiansPerSecond);
            if (speed > 0.0F)
                pose.rotation =
                    (Math::Quaternion::FromAxisAngle(body.angularVelocityRadiansPerSecond / speed, speed * seconds) * pose.rotation)
                        .Normalized();
        }
        return pose;
    }

    /** @brief Admits only rotation twist about Character up; platform tilt cannot alter capsule orientation. */
    [[nodiscard]] Result<Math::Quaternion> CarriedHeading(const Math::Quaternion heading, const Math::Vec3 up,
                                                          const Math::Quaternion rotation) {
        const auto projected = up * Math::Dot(Math::Vec3{rotation.x, rotation.y, rotation.z}, up);
        const Math::Quaternion twist{projected.x, projected.y, projected.z, rotation.w};
        const float squared = Math::LengthSquared(projected) + rotation.w * rotation.w;
        if (!std::isfinite(squared) || squared < 1.0e-12F)
            return Result<Math::Quaternion>::Failure(MakeError(CharacterErrors::RequestInvalid));
        return Result<Math::Quaternion>::Success((twist.Normalized() * heading).Normalized());
    }

    /** @brief Derives admitted kinematic angular velocity on the shortest arc in world space. */
    [[nodiscard]] Math::Vec3 TargetAngularVelocity(const Physics::PhysicsPose &previous, const Physics::PhysicsPose &target,
                                                   const float seconds) noexcept {
        const auto rotation = (target.rotation * previous.rotation.Inverse()).Normalized();
        const float sign = rotation.w < 0.0F ? -1.0F : 1.0F;
        const Math::Vec3 axis{rotation.x * sign, rotation.y * sign, rotation.z * sign};
        const float length = Math::Length(axis);
        return length > 1.0e-6F ? axis * (2.0F * std::atan2(length, rotation.w * sign) / (length * seconds)) : Math::Vec3{};
    }

    /** @brief Selects velocity at the actual departure point in its explicit start or transported body frame. */
    [[nodiscard]] Math::Vec3 PlatformLeaveVelocity(const CharacterPlatformBodyEvidence &motion, const Math::Vec3 point,
                                                   const Math::Vec3 bodyCenter,
                                                   const CharacterPlatformLeaveVelocityPolicy policy) noexcept {
        if (policy == CharacterPlatformLeaveVelocityPolicy::None)
            return {};
        if (policy == CharacterPlatformLeaveVelocityPolicy::Linear)
            return motion.linearVelocityMetersPerSecond;
        return motion.linearVelocityMetersPerSecond + Math::Cross(motion.angularVelocityRadiansPerSecond, point - bodyCenter);
    }

    /** @brief Computes finite carry and exactly one configured transfer from copied body and target evidence. */
    [[nodiscard]] Result<CharacterPlatformCarry> BuildPlatformCarry(const CharacterPlatformAttachment &attachment,
                                                                    const CharacterPlatformBodyEvidence &body,
                                                                    const CharacterTransformPublication &previous,
                                                                    const CharacterControllerDescriptor &descriptor,
                                                                    const CharacterFixedTickInput &input, const float maximum) {
        CharacterPlatformCarry result;
        result.heading = previous.heading;
        const float seconds = static_cast<float>(static_cast<double>(input.fixedDelta.ToNanoseconds()) / 1'000'000'000.0);
        if (!std::isfinite(seconds) || seconds <= 0.0F || !Math::IsFinite(body.linearVelocityMetersPerSecond) ||
            !Math::IsFinite(body.angularVelocityRadiansPerSecond))
            return Result<CharacterPlatformCarry>::Failure(MakeError(CharacterErrors::RequestInvalid));
        const auto target = PlatformTarget(body, input, seconds);
        if (const auto valid = Physics::ValidatePhysicsPose(target); valid.HasError())
            return Result<CharacterPlatformCarry>::Failure(valid.ErrorValue());
        result.predicted = body;
        result.predicted->pose = target;
        if (body.motion == Physics::PhysicsMotionType::Kinematic) {
            result.predicted->linearVelocityMetersPerSecond = (target.translation - body.pose.translation) / seconds;
            result.predicted->angularVelocityRadiansPerSecond = TargetAngularVelocity(body.pose, target, seconds);
        }
        if (body.motion != Physics::PhysicsMotionType::Static) {
            const auto targetPoint = target.translation + target.rotation.Rotate(attachment.localRoot.translation);
            result.displacement = targetPoint - previous.position;
            result.startLeaveVelocity =
                PlatformLeaveVelocity(*result.predicted, previous.position, body.pose.translation, descriptor.platformLeaveVelocity);
            result.leaveVelocity =
                PlatformLeaveVelocity(*result.predicted, targetPoint, target.translation, descriptor.platformLeaveVelocity);
            if (descriptor.inheritPlatformHeading) {
                auto heading =
                    CarriedHeading(previous.heading, descriptor.up, target.rotation * attachment.sampledBodyPose.rotation.Inverse());
                if (heading.HasError())
                    return Result<CharacterPlatformCarry>::Failure(heading.ErrorValue());
                result.heading = heading.Value();
            }
        }
        if (!Math::IsFinite(result.displacement) || !Math::IsFinite(result.leaveVelocity) || !Math::IsFinite(result.startLeaveVelocity) ||
            Math::Length(result.displacement) > maximum)
            return Result<CharacterPlatformCarry>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        return Result<CharacterPlatformCarry>::Success(result);
    }

    /** @brief Reads a previous exact binding once within the shared query budget; missing generations detach without stale velocity. */
    [[nodiscard]] Result<CharacterPlatformCarry> PreparePlatformCarry(auto &impl, const CharacterTransformPublication &previous,
                                                                      const std::optional<CharacterPlatformAttachment> &attachment,
                                                                      const CharacterControllerDescriptor &descriptor,
                                                                      const CharacterFixedTickInput &input) {
        CharacterPlatformCarry empty;
        empty.heading = previous.heading;
        if (!attachment || attachment->motion == Physics::PhysicsMotionType::Static)
            return Result<CharacterPlatformCarry>::Success(empty);
        if (!input.query.platformBody || !input.query.sweep || input.observer.movementResult)
            return Result<CharacterPlatformCarry>::Failure(MakeError(CharacterErrors::OperationUnsupported));
        const CharacterPhysicsQueryExpectations expected{impl.descriptor.sceneGeneration,        impl.descriptor.identity,
                                                         impl.descriptor.physicsWorld,           impl.descriptor.collisionFilterGeneration,
                                                         impl.descriptor.originGeneration,       input.tick,
                                                         impl.descriptor.physicsSnapshotRevision};
        if (const auto valid = ValidateCharacterPhysicsQueryContext(input.query, expected); valid.HasError())
            return Result<CharacterPlatformCarry>::Failure(valid.ErrorValue());
        CharacterMovementResult support;
        support.groundBody = attachment->body;
        support.groundShape = attachment->shape;
        auto evidence = ReadPlatformBody(impl, support, input);
        if (evidence.HasError())
            return Result<CharacterPlatformCarry>::Failure(evidence.ErrorValue());
        if (!evidence.Value()) {
            empty.stale = true;
            return Result<CharacterPlatformCarry>::Success(empty);
        }
        if (evidence.Value()->body != attachment->body || evidence.Value()->shape != attachment->shape) {
            empty.stale = true;
            return Result<CharacterPlatformCarry>::Success(empty);
        }
        if (const auto valid = ValidatePlatformBodyEvidence(*evidence.Value(), support); valid.HasError())
            return Result<CharacterPlatformCarry>::Failure(valid.ErrorValue());
        if (attachment->sourceTick >= input.tick || attachment->motion != evidence.Value()->motion)
            return Result<CharacterPlatformCarry>::Failure(MakeError(CharacterErrors::RequestInvalid));
        return BuildPlatformCarry(*attachment, *evidence.Value(), previous, descriptor, input,
                                  impl.settings.Values().work.maximumDisplacementMetersPerTick);
    }
}  // namespace Horo::Character::Detail
