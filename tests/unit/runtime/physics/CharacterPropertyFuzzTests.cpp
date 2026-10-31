#include "CharacterFoundationQualificationTestHelpers.h"
#include "CharacterPropertyCorpus.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/CharacterSceneComponents.h"

#include <bit>
#include <cmath>
#include <limits>

namespace Horo::Character {
    namespace {
        using namespace TestDetail;
        using PropertyTest::Recipe;

        /** @brief Uses disjoint malformed equivalence classes, including arbitrary payload NaNs and finite extremes. */
        float Malformed(const Recipe &input) {
            switch (input.words[1] % 6) {
                case 0:
                    return std::numeric_limits<float>::infinity();
                case 1:
                    return -std::numeric_limits<float>::infinity();
                case 2:
                    return std::bit_cast<float>(0x7fc00000U | (input.words[2] & 0x003fffffU));
                case 3:
                    return -std::numeric_limits<float>::max();
                case 4:
                    return -std::numeric_limits<float>::denorm_min();
                default:
                    return 0;
            }
        }

        CharacterControllerDescriptor GeneratedDescriptor(const Recipe &input) {
            CharacterWorldDescriptor owner{WorldDescriptor().sceneGeneration, WorldId(), PhysicsWorldId(), 91, 101, 1};
            auto descriptor = ControllerDescriptor(owner);
            descriptor.capsule = {0.125F + static_cast<float>(input.words[2] % 32) / 32,
                                  0.25F + static_cast<float>(input.words[3] % 32) / 16};
            descriptor.maximumSlopeDegrees = static_cast<float>(input.words[4] % 91);
            descriptor.maximumContacts = 1 + input.words[5] % MaximumCharacterContacts;
            descriptor.collisionRootPosition = {static_cast<float>(input.words[6] % 1024), 0, 0};
            descriptor.crouchedCapsule =
                Physics::PhysicsCapsuleShape{descriptor.capsule.radiusMeters, descriptor.capsule.cylindricalHalfHeightMeters / 2};
            return descriptor;
        }

        /** @brief Mutates exactly one independent contract field; the original remains admitted. */
        std::string DescriptorProperty(const Recipe &input) {
            const auto descriptor = GeneratedDescriptor(input);
            if (ValidateCharacterControllerDescriptor(descriptor).HasError())
                return "generated descriptor rejected";
            auto bad = descriptor;
            const ErrorCodeDescriptor *expected = &CharacterErrors::DescriptorInvalid;
            switch (input.words[0] % 12) {
                case 0:
                    bad.capsule.radiusMeters = Malformed(input);
                    break;
                case 1:
                    bad.capsule.cylindricalHalfHeightMeters = Malformed(input);
                    break;
                case 2:
                    bad.up = {};
                    break;
                case 3:
                    bad.gravity.x = std::bit_cast<float>(0x7f800000U | (input.words[2] & 0x007fffffU));
                    break;
                case 4:
                    bad.collisionProfile = {};
                    break;
                case 5:
                    bad.defaultMaterial.assetGeneration = 0;
                    break;
                case 6:
                    bad.maximumSlopeDegrees = 91 + static_cast<float>(input.words[2] % 100);
                    break;
                case 7:
                    bad.maximumContacts = MaximumCharacterContacts + 1 + input.words[2] % 1024;
                    expected = &CharacterErrors::CapacityExceeded;
                    break;
                case 8:
                    bad.sceneGeneration = 0;
                    expected = &CharacterErrors::WorldInvalid;
                    break;
                case 9:
                    bad.crouchedCapsule->radiusMeters *= 2;
                    break;
                case 10:
                    bad.steepSlopePolicy = static_cast<CharacterSteepSlopePolicy>(255);
                    break;
                default:
                    bad.skinWidthMeters = Malformed(input);
                    break;
            }
            const auto result = ValidateCharacterControllerDescriptor(bad);
            if (result.HasValue() || result.ErrorValue().code.Value() != expected->code.Value())
                return "descriptor mutation typed rejection";
            if (ValidateCharacterControllerDescriptor(descriptor).HasError())
                return "descriptor original changed";
            return {};
        }

        std::string RequestProperty(const Recipe &input) {
            const auto descriptor = GeneratedDescriptor(input);
            const CharacterControllerHandle handle{descriptor.sceneGeneration, descriptor.characterWorld, {input.words[2] % 64, 1}};
            auto request = Movement(handle, 1ULL + input.words[3], 1ULL + input.words[4]);
            if (input.words[0] % 3 != 0)
                request.desiredVelocityMetersPerSecond = Math::Vec3{static_cast<float>(input.words[5] % 100), 0, 0};
            const auto originalVelocity = request.desiredVelocityMetersPerSecond;
            request.desiredHeading = Math::Quaternion::Identity();
            if (ValidateCharacterMovementRequest(request, descriptor.sceneGeneration, descriptor.characterWorld).HasError())
                return "generated request rejected";
            auto bad = request;
            const ErrorCodeDescriptor *expected = &CharacterErrors::RequestInvalid;
            switch (input.words[0] % 6) {
                case 0:
                    bad.tick = 0;
                    break;
                case 1:
                    bad.sequence = 0;
                    break;
                case 2:
                    bad.desiredVelocityMetersPerSecond = Math::Vec3{0, 0, 0};
                    bad.desiredVelocityMetersPerSecond->x = std::bit_cast<float>(0x7f800000U | (input.words[2] & 0x007fffffU));
                    break;
                case 3:
                    bad.desiredHeading = Math::Quaternion{0, 0, 0, 0};
                    break;
                case 4:
                    bad.controller.slot.generation = 0;
                    expected = &CharacterErrors::HandleMalformed;
                    break;
                default:
                    bad.stance = static_cast<CharacterStanceIntent>(255);
                    expected = &CharacterErrors::OperationUnsupported;
                    break;
            }
            const auto result = ValidateCharacterMovementRequest(bad, descriptor.sceneGeneration, descriptor.characterWorld);
            if (result.HasValue() || result.ErrorValue().code.Value() != expected->code.Value())
                return "request mutation typed rejection";
            if (request.controller != handle || request.tick == 0 || request.desiredVelocityMetersPerSecond != originalVelocity)
                return "request original changed";
            return {};
        }

        /** @brief Exercises actual durable Character component schema versions without inventing a checkpoint format. */
        std::string ComponentProperty(const Recipe &input) {
            Runtime::CharacterControllerComponent component;
            component.id = Runtime::PhysicsComponentId{1ULL + input.words[2]};
            component.geometry.capsule = {0.25F, 0.5F};
            component.movement.maximumContacts = 1 + input.words[3] % MaximumCharacterContacts;
            if (Runtime::ValidateCharacterControllerComponent(component).HasError())
                return "generated component rejected";
            auto bad = component;
            bad.schemaVersion = input.words[0] % 2 == 0 ? 0 : Runtime::CharacterSceneComponentSchemaVersion + 1 + input.words[4] % 1024;
            const auto result = Runtime::ValidateCharacterControllerComponent(bad);
            if (result.HasValue() || result.ErrorValue().code.Value() != Physics::PhysicsErrors::DescriptorInvalid.code.Value())
                return "schema skew accepted";
            if (component.schemaVersion != Runtime::CharacterSceneComponentSchemaVersion)
                return "schema validation mutated original";
            return {};
        }

        std::string SnapshotProperty(const Recipe &input) {
            const auto descriptor = GeneratedDescriptor(input);
            CharacterLocomotionSnapshot snapshot;
            snapshot.controller = {descriptor.sceneGeneration, descriptor.characterWorld, {input.words[2] % 64, 1}};
            snapshot.tick = 1ULL + input.words[3];
            snapshot.stateRevision = 1ULL + input.words[4];
            snapshot.capsule = descriptor.capsule;
            snapshot.movement.controller = snapshot.controller;
            snapshot.movement.tick = snapshot.tick;
            snapshot.movement.sequence = 1ULL + input.words[5];
            snapshot.movement.finalPosition = descriptor.collisionRootPosition;
            snapshot.transform.controller = snapshot.controller;
            snapshot.transform.sourceTick = snapshot.tick;
            snapshot.transform.publicationRevision = 1ULL + input.words[6];
            snapshot.transform.position = snapshot.movement.finalPosition;
            if (ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasError())
                return "generated snapshot rejected";
            auto bad = snapshot;
            switch (input.words[0] % 8) {
                case 0:
                    bad.stateRevision = 0;
                    break;
                case 1:
                    ++bad.transform.sourceTick;
                    break;
                case 2:
                    ++bad.movement.tick;
                    break;
                case 3:
                    bad.movement.contactCount = std::numeric_limits<std::uint32_t>::max();
                    break;
                case 4:
                    bad.capsule.radiusMeters = Malformed(input);
                    break;
                case 5:
                    bad.transform.position.x = std::numeric_limits<float>::infinity();
                    break;
                case 6:
                    bad.stance = static_cast<CharacterStance>(255);
                    break;
                default:
                    bad.movement.termination = static_cast<CharacterMovementTermination>(255);
                    break;
            }
            const auto field = input.words[0] % 8;
            const auto &expected = field == 3                   ? CharacterErrors::CapacityExceeded
                                   : (field == 4 || field >= 6) ? CharacterErrors::DescriptorInvalid
                                                                : CharacterErrors::PlacementInvalid;
            const auto result = ValidateCharacterLocomotionSnapshot(bad, descriptor);
            if (result.HasValue() || result.ErrorValue().code.Value() != expected.code.Value())
                return "malformed snapshot typed rejection";
            if (ValidateCharacterLocomotionSnapshot(snapshot, descriptor).HasError())
                return "snapshot original changed";
            return {};
        }
    }  // namespace

    TEST_CASE("Generated Character descriptors reject malformed geometry and policy", "[physics][character][character-property][fuzz]") {
        PropertyTest::Corpus("descriptor", DescriptorProperty);
    }

    TEST_CASE("Generated Character requests retain optional intent and reject malformed orders",
              "[physics][character][character-property][fuzz]") {
        PropertyTest::Corpus("request", RequestProperty);
    }

    TEST_CASE("Generated Character components reject durable schema skew", "[physics][character][character-property][fuzz]") {
        PropertyTest::Corpus("component-schema", ComponentProperty);
    }

    TEST_CASE("Generated Character snapshots reject mixed publication state", "[physics][character][character-property][fuzz]") {
        PropertyTest::Corpus("snapshot", SnapshotProperty);
    }

    TEST_CASE("Character corpus minimization preserves failure and replay identity", "[physics][character][character-property]") {
        Recipe input{{32, 12345, 99, 11, 44, 22, 8, 7}};
        const auto check = [](const Recipe &candidate) {
            return candidate.words[2] >= 3 ? std::string{"synthetic boundary"} : std::string{};
        };
        const auto reduced = PropertyTest::Minimize(input, check(input), check);
        REQUIRE(check(reduced) == check(input));
        REQUIRE(reduced.words[2] < input.words[2]);
        REQUIRE(reduced.words[0] == 0);
        REQUIRE(reduced.words[7] == 0);
        PropertyTest::Generator first(994), second(994);
        REQUIRE(first.Input(7).words == second.Input(7).words);
    }
}  // namespace Horo::Character
