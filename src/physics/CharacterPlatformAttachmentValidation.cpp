#include "CharacterPlatformAttachmentValidation.h"

#include <cmath>

namespace Horo::Character::Detail {
    namespace {
        /** @brief Checks complete support and tick identity before any local-frame reconstruction. */
        [[nodiscard]] bool PlatformBindingMatches(const CharacterPlatformAttachment &attachment,
                                                  const CharacterMovementResult &result) noexcept {
            return result.grounded && !result.jumpApplied && result.groundBody == attachment.body &&
                   result.groundShape == attachment.shape && result.groundSubshape == attachment.subshape &&
                   attachment.sourceTick == result.tick && attachment.physicsSnapshotRevision != 0;
        }

        /** @brief Checks the closed support motion vocabulary against the controller's admission policy. */
        [[nodiscard]] bool PlatformMotionEligible(const Physics::PhysicsMotionType motion,
                                                  const CharacterControllerDescriptor &descriptor) noexcept {
            using enum Physics::PhysicsMotionType;
            return motion == Static || motion == Kinematic || (motion == Dynamic && descriptor.allowDynamicPlatformAttachment);
        }

        /** @brief Validates numeric evidence before quaternion rotation or point reconstruction. */
        [[nodiscard]] bool PlatformFrameFinite(const CharacterPlatformAttachment &attachment) {
            const auto normal = attachment.localContactNormal;
            const bool unitNormal = Math::IsFinite(normal) &&
                                    std::abs(static_cast<double>(Math::Dot(normal, normal)) - 1.0) <= CharacterUnitSquaredNormTolerance;
            return Math::IsFinite(attachment.localContactPoint) && unitNormal &&
                   Physics::ValidatePhysicsPose(attachment.localRoot).HasValue() &&
                   Physics::ValidatePhysicsPose(attachment.sampledBodyPose).HasValue();
        }

        /** @brief Requires local-frame reconstruction of the selected support and authoritative root. */
        [[nodiscard]] bool PlatformFrameMatches(const CharacterPlatformAttachment &attachment,
                                                const CharacterMovementResult &result) noexcept {
            const auto &pose = attachment.sampledBodyPose;
            constexpr float FrameToleranceMeters = 1.0e-3F;
            const auto root = pose.translation + pose.rotation.Rotate(attachment.localRoot.translation);
            const auto point = pose.translation + pose.rotation.Rotate(attachment.localContactPoint);
            const auto normal = pose.rotation.Rotate(attachment.localContactNormal);
            const auto heading = pose.rotation * attachment.localRoot.rotation;
            const float headingDot = heading.x * result.finalHeading.x + heading.y * result.finalHeading.y +
                                     heading.z * result.finalHeading.z + heading.w * result.finalHeading.w;
            return Math::Length(root - result.finalPosition) <= FrameToleranceMeters &&
                   Math::Length(point - result.groundPoint) <= FrameToleranceMeters &&
                   Math::Length(normal - result.groundNormal) <= 1.0e-5F && std::abs(std::abs(headingDot) - 1.0F) <= 1.0e-5F;
        }

        /** @brief Checks that an attachment outcome agrees with the presence of an owned movement base. */
        [[nodiscard]] bool PlatformOutcomeMatches(const CharacterMovementResult &result) noexcept {
            using enum CharacterPlatformAttachmentChange;
            if (result.platformAttachmentChange > Unavailable || result.platformAttached != result.platformAttachment.has_value())
                return false;
            const bool attachmentOutcome = result.platformAttachmentChange == Attached || result.platformAttachmentChange == BaseChanged;
            if (!result.platformAttachment)
                return !attachmentOutcome;
            return result.platformAttachmentChange == None || attachmentOutcome;
        }
    }  // namespace

    /** @copydoc ValidateCharacterPlatformAttachment */
    Result<void> ValidateCharacterPlatformAttachment(const CharacterMovementResult &result,
                                                     const CharacterControllerDescriptor &descriptor) {
        if (!PlatformOutcomeMatches(result))
            return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
        if (!result.platformAttachment)
            return Result<void>::Success();
        if (const auto &attachment = *result.platformAttachment;
            !PlatformBindingMatches(attachment, result) || !PlatformMotionEligible(attachment.motion, descriptor) ||
            !PlatformFrameFinite(attachment) || !PlatformFrameMatches(attachment, result))
            return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
        return Result<void>::Success();
    }
}  // namespace Horo::Character::Detail
