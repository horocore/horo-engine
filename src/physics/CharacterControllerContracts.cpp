#include "Horo/Physics/CharacterControllerContracts.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace Horo::Character {
    namespace {
        constexpr auto KnownCollisionFlags = CharacterCollisionFlags::Sides | CharacterCollisionFlags::Ground |
                                             CharacterCollisionFlags::Ceiling | CharacterCollisionFlags::Step;

        /** @brief Checks a finite unit vector against the public Character tolerance. */
        [[nodiscard]] bool IsUnit(const Math::Vec3 value) noexcept {
            return Math::IsFinite(value) &&
                   std::abs(static_cast<double>(Math::Dot(value, value)) - 1.0) <= CharacterUnitSquaredNormTolerance;
        }

        /** @brief Checks a finite unit quaternion without normalizing caller evidence. */
        [[nodiscard]] bool IsUnit(const Math::Quaternion value) noexcept {
            const double squaredNorm = static_cast<double>(value.x) * value.x + static_cast<double>(value.y) * value.y +
                                       static_cast<double>(value.z) * value.z + static_cast<double>(value.w) * value.w;
            return Math::IsFinite(value) && std::abs(squaredNorm - 1.0) <= CharacterUnitSquaredNormTolerance;
        }

        /** @brief Checks copied material identity and generation evidence. */
        [[nodiscard]] bool IsMaterialValid(const Physics::PhysicsQueryMaterial &material) noexcept {
            return material.asset.IsValid() && material.assetGeneration != 0 && material.slot.IsValid();
        }

        /** @brief Checks descriptor generation and paired world identities before field validation. */
        [[nodiscard]] Result<void> ValidateDescriptorWorlds(const CharacterControllerDescriptor &descriptor) {
            if (descriptor.sceneGeneration == 0 || !descriptor.characterWorld.IsValid() || !descriptor.physicsWorld.IsValid())
                return Result<void>::Failure(MakeError(CharacterErrors::WorldInvalid));
            return Result<void>::Success();
        }

        /** @brief Checks capsule and controller-space numeric basis fields. */
        [[nodiscard]] Result<void> ValidateDescriptorGeometry(const CharacterControllerDescriptor &descriptor) {
            if (const auto capsule = Physics::ValidatePhysicsShapeDescriptor(Physics::PhysicsShapeDescriptor{descriptor.capsule});
                capsule.HasError())
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Controller capsule dimensions are invalid."));
            if (!Math::IsFinite(descriptor.collisionRootPosition) || !IsUnit(descriptor.up) || !Math::IsFinite(descriptor.gravity))
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        /** @brief Checks canonical collision and material identities. */
        [[nodiscard]] Result<void> ValidateDescriptorBindings(const CharacterControllerDescriptor &descriptor) {
            if (!descriptor.collisionProfile.IsValid() || !descriptor.queryChannel.IsValid() ||
                !IsMaterialValid(descriptor.defaultMaterial))
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        /** @brief Checks finite non-negative controller tuning values. */
        [[nodiscard]] bool IsFiniteNonNegative(const float value) noexcept {
            return std::isfinite(value) && value >= 0;
        }

        /** @brief Checks controller scalar policy without changing capacity error precedence. */
        [[nodiscard]] Result<void> ValidateDescriptorPolicy(const CharacterControllerDescriptor &descriptor) {
            if (!std::isfinite(descriptor.skinWidthMeters) || descriptor.skinWidthMeters <= 0 ||
                !IsFiniteNonNegative(descriptor.minimumMoveDistanceMeters) || !IsFiniteNonNegative(descriptor.maximumStepHeightMeters) ||
                !IsFiniteNonNegative(descriptor.maximumSlopeDegrees) || descriptor.maximumSlopeDegrees > 90)
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid));
            if (descriptor.maximumContacts == 0 || descriptor.maximumContacts > MaximumCharacterContacts)
                return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Checks request tick and optional numeric intent before stance support. */
        [[nodiscard]] Result<void> ValidateRequestIntent(const CharacterMovementRequest &request) {
            if (request.tick == 0 || request.sequence == 0 ||
                (request.desiredVelocityMetersPerSecond.has_value() && !Math::IsFinite(*request.desiredVelocityMetersPerSecond)) ||
                (request.desiredHeading.has_value() && !IsUnit(*request.desiredHeading)))
                return Result<void>::Failure(MakeError(CharacterErrors::RequestInvalid));
            return Result<void>::Success();
        }

        /** @brief Checks that a stance discriminator is one of the supported typed values. */
        [[nodiscard]] Result<void> ValidateStance(const CharacterStanceIntent stance) {
            switch (stance) {
                using enum CharacterStanceIntent;
                case Keep:
                case Stand:
                case Crouch:
                    return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported));
        }

        /** @brief Checks movement metadata before flags, capacities and surface evidence. */
        [[nodiscard]] Result<void> ValidateResultMetadata(const CharacterMovementResult &result) {
            if (result.tick == 0 || result.sequence == 0 || !Math::IsFinite(result.finalPosition) || !IsUnit(result.finalHeading) ||
                !Math::IsFinite(result.achievedVelocityMetersPerSecond) || !IsUnit(result.up) ||
                !std::isfinite(result.groundSlopeDegrees) || result.groundSlopeDegrees < 0 || result.groundSlopeDegrees > 180)
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Movement result metadata is invalid."));
            return Result<void>::Success();
        }

        /** @brief Checks collision bits and admitted contact/truncation bounds. */
        [[nodiscard]] Result<void> ValidateResultBounds(const CharacterMovementResult &result,
                                                        const CharacterControllerDescriptor &descriptor) {
            using FlagValue = std::underlying_type_t<CharacterCollisionFlags>;
            if ((static_cast<FlagValue>(result.collisions) & ~static_cast<FlagValue>(KnownCollisionFlags)) != 0)
                return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported));
            if (result.contactCount > descriptor.maximumContacts || result.contactCount > result.contacts.size())
                return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded));
            if (result.truncated && result.contactCount != descriptor.maximumContacts)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Truncation requires a full admitted contact prefix."));
            return Result<void>::Success();
        }

        /** @brief Checks coherent grounded or airborne surface evidence. */
        [[nodiscard]] Result<void> ValidateGroundEvidence(const CharacterMovementResult &result) {
            using FlagValue = std::underlying_type_t<CharacterCollisionFlags>;
            const bool hasGroundCollision =
                (static_cast<FlagValue>(result.collisions) & static_cast<FlagValue>(CharacterCollisionFlags::Ground)) != 0;
            if (result.grounded) {
                if (!IsUnit(result.groundNormal) || !result.groundMaterial.has_value() || !IsMaterialValid(*result.groundMaterial))
                    return Result<void>::Failure(
                        MakeError(CharacterErrors::DescriptorInvalid, "Grounded result lacks valid surface evidence."));
                if (!hasGroundCollision)
                    return Result<void>::Failure(
                        MakeError(CharacterErrors::DescriptorInvalid, "Grounded result lacks ground collision evidence."));
            } else if (result.groundMaterial.has_value()) {
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Airborne result cannot claim ground material evidence."));
            }
            if (result.platformAttached && !result.grounded)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Platform attachment requires grounded evidence."));
            if (result.groundingRevalidationRequired && (result.grounded || result.platformAttached))
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Grounding revalidation cannot coexist with support state."));
            return Result<void>::Success();
        }

        /** @brief Validates one active contact against the descriptor's Physics world. */
        [[nodiscard]] Result<void> ValidateContact(const CharacterSurfaceContact &contact, const Physics::PhysicsWorldId expectedWorld) {
            if (!contact.shape.IsValid() || contact.shape.world != expectedWorld)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Contact shape does not belong to the descriptor world."));
            if (contact.body.has_value()) {
                const auto owner = Physics::ValidatePhysicsHandleOwner(*contact.body, expectedWorld);
                if (owner.HasError())
                    return Result<void>::Failure(
                        MakeError(CharacterErrors::DescriptorInvalid, "Contact body does not belong to the descriptor world."));
            }
            if (!Math::IsFinite(contact.point) || !IsUnit(contact.normal) || !IsMaterialValid(contact.material) ||
                !std::isfinite(contact.penetrationDepthMeters) || contact.penetrationDepthMeters < 0.0F)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Contact evidence contains invalid numeric or material data."));
            return Result<void>::Success();
        }

        /** @brief Checks the closed response vocabulary accepted by a Character sweep adapter. */
        [[nodiscard]] bool IsSweepResponseSupported(const Physics::PhysicsQueryResponse response) noexcept {
            return response == Physics::PhysicsQueryResponse::Overlap || response == Physics::PhysicsQueryResponse::Block;
        }

        /** @brief Validates one complete sweep request before adapter evidence is consumed. */
        [[nodiscard]] Result<void> ValidateSweepRequest(const CharacterSweepProbeRequest &request) {
            if (const auto owner =
                    ValidateCharacterControllerHandleOwner(request.controller, request.sceneGeneration, request.characterWorld);
                owner.HasError())
                return owner;
            if (!request.physicsWorld.IsValid() || !request.collisionProfile.IsValid() || !request.queryChannel.IsValid() ||
                !Math::IsFinite(request.position) || !IsUnit(request.up) || !IsUnit(request.direction) ||
                !std::isfinite(request.maximumDistanceMeters) || request.maximumDistanceMeters <= 0.0F)
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Character sweep request is malformed."));
            if (const auto capsule = Physics::ValidatePhysicsShapeDescriptor(Physics::PhysicsShapeDescriptor{request.capsule});
                capsule.HasError())
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Character sweep capsule is invalid."));
            return Result<void>::Success();
        }

        /** @brief Validates one copied hit against the request's exact world and travel bound. */
        [[nodiscard]] Result<void> ValidateSweepHit(const CharacterSweepHit &hit, const CharacterSweepProbeRequest &request) {
            if (!hit.shape.IsValid() || hit.shape.world != request.physicsWorld)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::DescriptorInvalid, "Character sweep hit shape does not belong to the request world."));
            if (hit.body.has_value()) {
                if (const auto owner = Physics::ValidatePhysicsHandleOwner(*hit.body, request.physicsWorld); owner.HasError())
                    return Result<void>::Failure(
                        MakeError(CharacterErrors::DescriptorInvalid, "Character sweep hit body does not belong to the request world."));
            }
            if (!IsSweepResponseSupported(hit.response) || !Math::IsFinite(hit.point) || !IsUnit(hit.normal) ||
                !std::isfinite(hit.distanceMeters) || hit.distanceMeters < 0.0F || hit.distanceMeters > request.maximumDistanceMeters)
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Character sweep hit evidence is malformed."));
            if (hit.material.has_value() && !IsMaterialValid(*hit.material))
                return Result<void>::Failure(MakeError(CharacterErrors::DescriptorInvalid, "Character sweep hit material is malformed."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CharacterWorldId::Create */
    Result<CharacterWorldId> CharacterWorldId::Create(const std::uint64_t value) {
        if (value == 0)
            return Result<CharacterWorldId>::Failure(MakeError(CharacterErrors::WorldInvalid));
        return Result<CharacterWorldId>::Success(CharacterWorldId{value});
    }

    /** @copydoc CharacterWorldId::Value */
    std::uint64_t CharacterWorldId::Value() const noexcept {
        return value_;
    }

    /** @copydoc CharacterWorldId::IsValid */
    bool CharacterWorldId::IsValid() const noexcept {
        return value_ != 0;
    }

    /** @copydoc ValidateCharacterControllerHandleOwner */
    Result<void> ValidateCharacterControllerHandleOwner(const CharacterControllerHandle &handle,
                                                        const std::uint64_t expectedSceneGeneration, const CharacterWorldId expectedWorld) {
        if (expectedSceneGeneration == 0 || !expectedWorld.IsValid())
            return Result<void>::Failure(MakeError(CharacterErrors::WorldInvalid));
        if (!handle.IsValid())
            return Result<void>::Failure(MakeError(CharacterErrors::HandleMalformed));
        if (handle.sceneGeneration != expectedSceneGeneration || handle.world != expectedWorld)
            return Result<void>::Failure(MakeError(CharacterErrors::HandleWorldMismatch));
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterControllerDescriptor */
    Result<void> ValidateCharacterControllerDescriptor(const CharacterControllerDescriptor &descriptor) {
        if (const auto worlds = ValidateDescriptorWorlds(descriptor); worlds.HasError())
            return worlds;
        if (const auto geometry = ValidateDescriptorGeometry(descriptor); geometry.HasError())
            return geometry;
        if (const auto bindings = ValidateDescriptorBindings(descriptor); bindings.HasError())
            return bindings;
        return ValidateDescriptorPolicy(descriptor);
    }

    /** @copydoc ValidateCharacterPhysicsQueryContext */
    Result<void> ValidateCharacterPhysicsQueryContext(const CharacterPhysicsQueryContext &context,
                                                      const CharacterPhysicsQueryExpectations &expected) {
        if (const std::array expectedValid{expected.sceneGeneration != 0, expected.characterWorld.IsValid(),
                                           expected.physicsWorld.IsValid(), expected.collisionFilterGeneration != 0,
                                           expected.originGeneration != 0, expected.physicsSnapshotRevision != 0};
            !std::ranges::all_of(expectedValid, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::WorldInvalid));
        if (const std::array ownerMatches{context.sceneGeneration == expected.sceneGeneration,
                                          context.characterWorld == expected.characterWorld, context.physicsWorld == expected.physicsWorld};
            !std::ranges::all_of(ownerMatches, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::HandleWorldMismatch));
        if (const std::array snapshotMatches{context.collisionFilterGeneration == expected.collisionFilterGeneration,
                                             context.originGeneration == expected.originGeneration, context.tick == expected.tick,
                                             context.physicsSnapshotRevision == expected.physicsSnapshotRevision};
            !std::ranges::all_of(snapshotMatches, std::identity{}))
            return Result<void>::Failure(MakeError(CharacterErrors::QuerySnapshotStale));
        if (context.overlap == nullptr && context.sweep == nullptr)
            return Result<void>::Failure(
                MakeError(CharacterErrors::OperationUnsupported, "Character movement requires a Physics overlap or sweep probe."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterOverlapProbeResult */
    Result<void> ValidateCharacterOverlapProbeResult(const CharacterOverlapProbeResult &result) {
        if (!Math::IsFinite(result.recoveryDisplacement))
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid, "Overlap recovery displacement must be finite."));
        if (result.overlapCount == 0) {
            if (Math::LengthSquared(result.recoveryDisplacement) > Math::DefaultEpsilon * Math::DefaultEpsilon)
                return Result<void>::Failure(
                    MakeError(CharacterErrors::PlacementInvalid, "A clear overlap probe cannot provide a recovery displacement."));
            return Result<void>::Success();
        }
        if (Math::LengthSquared(result.recoveryDisplacement) <= Math::DefaultEpsilon * Math::DefaultEpsilon)
            return Result<void>::Failure(
                MakeError(CharacterErrors::OverlapRecoveryFailed, "Overlapping geometry did not provide a depenetration displacement."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterSweepProbeResult */
    Result<void> ValidateCharacterSweepProbeResult(const CharacterSweepProbeResult &result, const CharacterSweepProbeRequest &request) {
        if (const auto requestValidation = ValidateSweepRequest(request); requestValidation.HasError())
            return requestValidation;
        if (result.hitCount > result.hits.size())
            return Result<void>::Failure(MakeError(CharacterErrors::CapacityExceeded, "Character sweep exceeded its fixed hit bound."));
        if (result.truncated && result.hitCount != result.hits.size())
            return Result<void>::Failure(
                MakeError(CharacterErrors::DescriptorInvalid, "Character sweep truncation requires a full retained hit prefix."));
        for (std::uint32_t index = 0; index < result.hitCount; ++index) {
            if (const auto hit = ValidateSweepHit(result.hits[index], request); hit.HasError())
                return hit;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterTransformPublication */
    Result<void> ValidateCharacterTransformPublication(const CharacterTransformPublication &publication,
                                                       const std::uint64_t expectedSceneGeneration,
                                                       const CharacterWorldId expectedCharacterWorld) {
        if (const auto owner =
                ValidateCharacterControllerHandleOwner(publication.controller, expectedSceneGeneration, expectedCharacterWorld);
            owner.HasError())
            return owner;
        if (publication.authority != CharacterTransformAuthority::CharacterController)
            return Result<void>::Failure(MakeError(CharacterErrors::OperationUnsupported, "Unknown Character transform authority."));
        if (publication.publicationRevision == 0 || !Math::IsFinite(publication.position) || !IsUnit(publication.heading) ||
            !IsUnit(publication.up) || (publication.platformAttached && !publication.grounded) ||
            (publication.groundingRevalidationRequired && (publication.grounded || publication.platformAttached)))
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid, "Character transform publication is incoherent."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterTeleportRequest */
    Result<void> ValidateCharacterTeleportRequest(const CharacterTeleportRequest &request, const std::uint64_t expectedSceneGeneration,
                                                  const CharacterWorldId expectedWorld) {
        if (const auto owner = ValidateCharacterControllerHandleOwner(request.controller, expectedSceneGeneration, expectedWorld);
            owner.HasError())
            return owner;
        if (request.tick == 0 || !Math::IsFinite(request.targetPosition) || !IsUnit(request.targetHeading))
            return Result<void>::Failure(
                MakeError(CharacterErrors::RequestInvalid, "Teleport requires a positive tick, finite position and unit heading."));
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterMovementRequest */
    Result<void> ValidateCharacterMovementRequest(const CharacterMovementRequest &request, const std::uint64_t expectedSceneGeneration,
                                                  const CharacterWorldId expectedWorld) {
        if (auto owner = ValidateCharacterControllerHandleOwner(request.controller, expectedSceneGeneration, expectedWorld);
            owner.HasError())
            return owner;
        if (const auto intent = ValidateRequestIntent(request); intent.HasError())
            return intent;
        return ValidateStance(request.stance);
    }

    /** @copydoc ValidateCharacterMovementResult */
    Result<void> ValidateCharacterMovementResult(const CharacterMovementResult &result, const CharacterControllerDescriptor &descriptor) {
        if (const auto descriptorValidation = ValidateCharacterControllerDescriptor(descriptor); descriptorValidation.HasError())
            return descriptorValidation;
        if (auto owner = ValidateCharacterControllerHandleOwner(result.controller, descriptor.sceneGeneration, descriptor.characterWorld);
            owner.HasError())
            return owner;
        if (const auto metadata = ValidateResultMetadata(result); metadata.HasError())
            return metadata;
        if (const auto bounds = ValidateResultBounds(result, descriptor); bounds.HasError())
            return bounds;
        if (const auto ground = ValidateGroundEvidence(result); ground.HasError())
            return ground;
        for (std::uint32_t index = 0; index < result.contactCount; ++index) {
            const auto contact = ValidateContact(result.contacts[index], descriptor.physicsWorld);
            if (contact.HasError())
                return contact;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateCharacterLocomotionSnapshot */
    Result<void> ValidateCharacterLocomotionSnapshot(const CharacterLocomotionSnapshot &snapshot,
                                                     const CharacterControllerDescriptor &descriptor) {
        if (const auto descriptorValidation = ValidateCharacterControllerDescriptor(descriptor); descriptorValidation.HasError())
            return descriptorValidation;
        if (const auto owner =
                ValidateCharacterControllerHandleOwner(snapshot.controller, descriptor.sceneGeneration, descriptor.characterWorld);
            owner.HasError())
            return owner;
        if (snapshot.stateRevision == 0)
            return Result<void>::Failure(MakeError(CharacterErrors::PlacementInvalid, "Locomotion state revision is invalid."));
        if (const auto movement = ValidateCharacterMovementResult(snapshot.movement, descriptor); movement.HasError())
            return movement;
        if (const auto transform =
                ValidateCharacterTransformPublication(snapshot.transform, descriptor.sceneGeneration, descriptor.characterWorld);
            transform.HasError())
            return transform;
        if (snapshot.movement.controller != snapshot.controller || snapshot.transform.controller != snapshot.controller ||
            snapshot.movement.tick != snapshot.tick || snapshot.transform.sourceTick != snapshot.tick ||
            snapshot.movement.finalPosition != snapshot.transform.position ||
            snapshot.movement.finalHeading != snapshot.transform.heading || snapshot.movement.up != snapshot.transform.up ||
            snapshot.movement.grounded != snapshot.transform.grounded ||
            snapshot.movement.platformAttached != snapshot.transform.platformAttached ||
            snapshot.movement.groundingRevalidationRequired != snapshot.transform.groundingRevalidationRequired)
            return Result<void>::Failure(
                MakeError(CharacterErrors::PlacementInvalid, "Locomotion state and transform publication do not match."));
        return Result<void>::Success();
    }
}  // namespace Horo::Character
