#include "Horo/Physics/CharacterControllerContracts.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <type_traits>

namespace Horo::Character {
    namespace {
        CharacterWorldId CharacterWorld(const std::uint64_t value = 7) {
            const auto result = CharacterWorldId::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Physics::PhysicsWorldId PhysicsWorld(const std::uint64_t value = 11) {
            const auto result = Physics::PhysicsWorldId::Create(value);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        Physics::PhysicsQueryMaterial Material() {
            return {
                Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                3,
                Physics::PhysicsMaterialSlotId::FromValue(5),
            };
        }

        CharacterControllerDescriptor Descriptor() {
            CharacterControllerDescriptor descriptor;
            descriptor.sceneGeneration = 13;
            descriptor.characterWorld = CharacterWorld();
            descriptor.physicsWorld = PhysicsWorld();
            descriptor.collisionProfile = Physics::CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
            descriptor.queryChannel = Physics::PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
            descriptor.defaultMaterial = Material();
            return descriptor;
        }

        CharacterControllerHandle Controller(const CharacterControllerDescriptor &descriptor) {
            return {descriptor.sceneGeneration, descriptor.characterWorld, {2, 4}};
        }

        CharacterSurfaceContact Contact(const CharacterControllerDescriptor &descriptor) {
            CharacterSurfaceContact contact;
            contact.body = Physics::BodyHandle{descriptor.physicsWorld, {3, 6}};
            contact.shape = {descriptor.physicsWorld, {4, 8}};
            contact.material = Material();
            return contact;
        }

        CharacterMovementResult MovementResult(const CharacterControllerDescriptor &descriptor) {
            CharacterMovementResult result;
            result.controller = Controller(descriptor);
            result.tick = 17;
            result.sequence = 9;
            result.contacts[0] = Contact(descriptor);
            result.contactCount = 1;
            return result;
        }

        void RequireError(const Result<void> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().domain.Value() == expected.domain.Value());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }

        TEST_CASE("Character identities retain scene world slot and generation", "[physics][character][identity]") {
            REQUIRE_FALSE(CharacterWorldId{}.IsValid());
            RequireError(Result<void>::Failure(CharacterWorldId::Create(0).ErrorValue()), CharacterErrors::WorldInvalid);
            const auto descriptor = Descriptor();
            const auto handle = Controller(descriptor);
            REQUIRE(handle.IsValid());
            REQUIRE(ValidateCharacterControllerHandleOwner(handle, descriptor.sceneGeneration, descriptor.characterWorld).HasValue());
            RequireError(ValidateCharacterControllerHandleOwner({}, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::HandleMalformed);
            RequireError(ValidateCharacterControllerHandleOwner(handle, 0, descriptor.characterWorld), CharacterErrors::WorldInvalid);
            RequireError(ValidateCharacterControllerHandleOwner(handle, descriptor.sceneGeneration + 1, descriptor.characterWorld),
                         CharacterErrors::HandleWorldMismatch);
            RequireError(ValidateCharacterControllerHandleOwner(handle, descriptor.sceneGeneration, CharacterWorld(8)),
                         CharacterErrors::HandleWorldMismatch);
            static_assert(std::is_trivially_copyable_v<CharacterControllerHandle>);
        }

        TEST_CASE("Character descriptor accepts exact boundary policy without ambient work", "[physics][character][descriptor]") {
            auto descriptor = Descriptor();
            descriptor.minimumMoveDistanceMeters = 0;
            descriptor.maximumStepHeightMeters = 0;
            descriptor.maximumSlopeDegrees = 0;
            descriptor.maximumContacts = MaximumCharacterContacts;
            REQUIRE(ValidateCharacterControllerDescriptor(descriptor).HasValue());
            descriptor.maximumSlopeDegrees = 90;
            REQUIRE(ValidateCharacterControllerDescriptor(descriptor).HasValue());
            REQUIRE(descriptor.characterWorld == CharacterWorld());
            REQUIRE(descriptor.physicsWorld == PhysicsWorld());
        }

        TEST_CASE("Character descriptor rejects malformed geometry basis filters and numeric policy", "[physics][character][descriptor]") {
            auto descriptor = Descriptor();
            descriptor.capsule.radiusMeters = 0;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = Descriptor();
            descriptor.up = {};
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = Descriptor();
            descriptor.gravity.x = std::numeric_limits<float>::infinity();
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = Descriptor();
            descriptor.queryChannel = {};
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = Descriptor();
            descriptor.skinWidthMeters = 0;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
            descriptor = Descriptor();
            descriptor.maximumSlopeDegrees = 91;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::DescriptorInvalid);
        }

        TEST_CASE("Character descriptor enforces one fixed contact ceiling", "[physics][character][capacity]") {
            auto descriptor = Descriptor();
            descriptor.maximumContacts = 0;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::CapacityExceeded);
            descriptor.maximumContacts = MaximumCharacterContacts + 1;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::CapacityExceeded);
        }

        TEST_CASE("Character request preserves explicit absence and zero movement intent", "[physics][character][request]") {
            const auto descriptor = Descriptor();
            CharacterMovementRequest request{Controller(descriptor), 19, 1};
            REQUIRE_FALSE(request.desiredVelocityMetersPerSecond.has_value());
            REQUIRE(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld).HasValue());
            request.desiredVelocityMetersPerSecond = Math::Vec3{};
            request.desiredHeading = Math::Quaternion{};
            request.jumpRequested = true;
            request.stance = CharacterStanceIntent::Crouch;
            REQUIRE(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld).HasValue());
            REQUIRE(request.desiredVelocityMetersPerSecond == Math::Vec3{});
        }

        TEST_CASE("Character request rejects stale-shaped owner numeric and enum input without mutation", "[physics][character][request]") {
            const auto descriptor = Descriptor();
            CharacterMovementRequest request{Controller(descriptor), 0, 1, Math::Vec3{1, 2, 3}};
            const auto velocity = request.desiredVelocityMetersPerSecond;
            RequireError(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::RequestInvalid);
            REQUIRE(request.desiredVelocityMetersPerSecond == velocity);
            request.tick = 1;
            request.desiredVelocityMetersPerSecond->x = std::numeric_limits<float>::quiet_NaN();
            RequireError(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::RequestInvalid);
            request.desiredVelocityMetersPerSecond = {};
            request.stance = static_cast<CharacterStanceIntent>(255);
            RequireError(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::OperationUnsupported);
            request.stance = CharacterStanceIntent::Keep;
            request.controller.slot.generation = 0;
            RequireError(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::HandleMalformed);
        }

        TEST_CASE("Character result owns bounded ordered contacts and grounded surface evidence", "[physics][character][result]") {
            const auto descriptor = Descriptor();
            auto result = MovementResult(descriptor);
            result.grounded = true;
            result.groundMaterial = Material();
            result.collisions = CharacterCollisionFlags::Ground | CharacterCollisionFlags::Sides;
            REQUIRE(ValidateCharacterMovementResult(result, descriptor).HasValue());
            REQUIRE(result.contactCount == 1);
            REQUIRE(result.contacts[0].body.has_value());
            REQUIRE(result.contacts[0].body->slot.index == 3);
            result.grounded = false;
            result.groundMaterial.reset();
            REQUIRE(ValidateCharacterMovementResult(result, descriptor).HasValue());
        }

        TEST_CASE("Character result rejects overflow unknown flags and incoherent surface evidence transactionally",
                  "[physics][character][result]") {
            auto descriptor = Descriptor();
            descriptor.maximumContacts = 1;
            auto result = MovementResult(descriptor);
            const auto originalPosition = result.finalPosition;
            result.contactCount = 2;
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::CapacityExceeded);
            REQUIRE(result.finalPosition == originalPosition);
            result.contactCount = 1;
            result.collisions = static_cast<CharacterCollisionFlags>(1U << 15U);
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::OperationUnsupported);
            result.collisions = CharacterCollisionFlags::None;
            result.contactCount = 0;
            result.truncated = true;
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::DescriptorInvalid);
            result.contactCount = 1;
            result.truncated = false;
            result.grounded = true;
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::DescriptorInvalid);
            result.grounded = false;
            result.contacts[0].shape.world = PhysicsWorld(12);
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::DescriptorInvalid);
            REQUIRE(result.contacts[0].shape.world == PhysicsWorld(12));
        }

        TEST_CASE("Character validation partitions preserve public error precedence", "[physics][character][validation]") {
            auto descriptor = Descriptor();
            descriptor.sceneGeneration = 0;
            descriptor.capsule.radiusMeters = 0;
            RequireError(ValidateCharacterControllerDescriptor(descriptor), CharacterErrors::WorldInvalid);

            descriptor = Descriptor();
            CharacterMovementRequest request{{}, 0, 0};
            RequireError(ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld),
                         CharacterErrors::HandleMalformed);

            auto result = MovementResult(descriptor);
            result.controller = {};
            result.tick = 0;
            result.collisions = static_cast<CharacterCollisionFlags>(1U << 15U);
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::HandleMalformed);

            result.controller = Controller(descriptor);
            RequireError(ValidateCharacterMovementResult(result, descriptor), CharacterErrors::DescriptorInvalid);
        }

        TEST_CASE("Character errors expose stable actionable identities", "[physics][character][errors]") {
            const std::array<const ErrorCodeDescriptor *, 11> descriptors{{
                &CharacterErrors::WorldInvalid,
                &CharacterErrors::HandleMalformed,
                &CharacterErrors::HandleWorldMismatch,
                &CharacterErrors::HandleStale,
                &CharacterErrors::DescriptorInvalid,
                &CharacterErrors::RequestInvalid,
                &CharacterErrors::CommandOrderInvalid,
                &CharacterErrors::CapacityExceeded,
                &CharacterErrors::GenerationExhausted,
                &CharacterErrors::InvalidState,
                &CharacterErrors::OperationUnsupported,
            }};
            std::set<std::string_view> unique;
            for (const auto *descriptor : descriptors) {
                REQUIRE(descriptor->domain.Value() == "horo.character");
                REQUIRE(unique.insert(descriptor->code.Value()).second);
                REQUIRE_FALSE(descriptor->summary.empty());
                REQUIRE_FALSE(descriptor->remediationHint.empty());
            }
            REQUIRE(CharacterErrors::CapacityExceeded.userActionable);
            REQUIRE(CharacterErrors::HandleStale.code.Value() == "character.handle.stale");
        }
    }  // namespace
}  // namespace Horo::Character
