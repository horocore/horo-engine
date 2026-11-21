#pragma once

#include "CharacterWorldSweepInternal.h"

namespace Horo::Character::Detail {
    /** @brief Resolved intent after selecting the actual heading and translation basis. */
    struct CharacterRootMotionPlan final {
        Math::Quaternion basis{};
        Math::Quaternion twist{};
        Math::Vec3 worldTravel{};
        bool translate{};
        bool overrideTranslation{};
        bool rotate{};
    };

    /** @brief Pure swing contributes no capsule twist. */
    inline Math::Quaternion CharacterRootTwist(Math::Quaternion rotation, Math::Vec3 localUp) noexcept {
        const auto projected = localUp * Math::Dot(Math::Vec3{rotation.x, rotation.y, rotation.z}, localUp);
        const float squared = Math::LengthSquared(projected) + rotation.w * rotation.w;
        if (squared < 1.0e-12F)
            return {};
        const Math::Quaternion twist{projected.x, projected.y, projected.z, rotation.w};
        return twist.Normalized();
    }

    /** @brief Selects the desired root modes after platform heading has been admitted. */
    inline CharacterRootMotionPlan PlanRootMotion(const CharacterMovementRequest &command,
                                                  const CharacterTransformPublication &carried) noexcept {
        if (!command.rootMotion.has_value())
            return {};
        const auto &intent = *command.rootMotion;
        CharacterRootMotionPlan plan;
        plan.overrideTranslation = intent.translation == CharacterRootTranslation::Override;
        plan.translate = plan.overrideTranslation || intent.translation == CharacterRootTranslation::Additive ||
                         (intent.translation == CharacterRootTranslation::Suggest && !command.desiredVelocityMetersPerSecond.has_value());
        plan.basis =
            intent.rotation == CharacterRootRotation::Override ? carried.heading : command.desiredHeading.value_or(carried.heading);
        if (plan.translate)
            plan.worldTravel = plan.basis.Rotate(intent.delta.translation);
        plan.rotate = intent.rotation != CharacterRootRotation::GameplayOnly;
        if (plan.rotate)
            plan.twist = CharacterRootTwist(intent.delta.rotation, plan.basis.Inverse().Rotate(carried.up));
        return plan;
    }

    /** @brief Converts the attributed world sweep displacement back to the source frame. */
    [[nodiscard]] inline Result<void> FinishRootMotion(CharacterMovementResult &result, const CharacterMovementRequest &command,
                                                       const CharacterRootMotionPlan &plan, Math::Vec3 appliedWorld) {
        if (!command.rootMotion.has_value())
            return Result<void>::Success();
        if (!Math::IsFinite(appliedWorld))
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid));
        const auto &intent = *command.rootMotion;
        const auto applied = plan.basis.Inverse().Rotate(appliedWorld);
        const auto inverse = plan.twist.Inverse();
        result.rootMotion = CharacterRootMotionFeedback{intent.source,
                                                        {applied, plan.twist},
                                                        {inverse.Rotate(intent.delta.translation - applied),
                                                         (inverse * intent.delta.rotation).Normalized()}};
        return Result<void>::Success();
    }
}  // namespace Horo::Character::Detail
