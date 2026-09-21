#include "AllocationProbe.h"
#include "CharacterFastPathStorage.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

namespace Horo::Character::Detail {
    namespace {
        CharacterWorldSettings Settings() {
            CharacterWorldSettingsDescriptor descriptor;
            descriptor.capacities.maximumControllers = 2;
            descriptor.capacities.maximumQueuedCommands = 2;
            descriptor.capacities.maximumRetainedContacts = 2;
            descriptor.capacities.maximumQueuedEvents = 2;
            descriptor.capacities.maximumQueuedQueries = 2;
            descriptor.capacities.maximumStagedImpulses = 2;
            descriptor.work.maximumCommandsPerTick = 2;
            descriptor.work.maximumQueriesPerTick = 2;
            descriptor.work.maximumContactsPerMovement = 1;
            descriptor.work.scratchBytes = 64;
            const auto settings = CharacterWorldSettings::Capture(descriptor);
            REQUIRE(settings.HasValue());
            return settings.Value();
        }

        Physics::PhysicsWorldId World() {
            return Physics::PhysicsWorldId::Create(17).Value();
        }

        CharacterControllerHandle Controller(const std::uint32_t index) {
            return {31, CharacterWorldId::Create(19).Value(), {index, 1}};
        }

        CharacterSurfaceContact Contact(const std::uint32_t index, const float penetration) {
            const auto world = World();
            return {.body = Physics::BodyHandle{world, {index, 1}},
                    .shape = Physics::ShapeHandle{world, {index + 1, 1}},
                    .point = {static_cast<float>(index), 0, 0},
                    .normal = {0, 1, 0},
                    .material = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 1,
                                 Physics::PhysicsMaterialSlotId::FromValue(1)},
                    .penetrationDepthMeters = penetration};
        }

        Physics::PhysicsQueryHit Hit(const std::uint32_t index, const float distance) {
            const auto world = World();
            return {.body = Physics::BodyHandle{world, {index, 1}},
                    .shape = Physics::ShapeHandle{world, {index + 1, 1}},
                    .subshape = {},
                    .material = {},
                    .layer = Physics::CollisionLayerId::Parse("22345678-1234-4234-8234-123456789abc").Value(),
                    .profile = Physics::CollisionProfileId::Parse("32345678-1234-4234-8234-123456789abc").Value(),
                    .channel = Physics::PhysicsQueryChannelId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                    .filterSchemaGeneration = 1,
                    .response = Physics::PhysicsQueryResponse::Block,
                    .position = {distance, 0, 0},
                    .normal = Math::Vec3{0, 1, 0},
                    .distanceMeters = distance};
        }

        CharacterFastPathEvent Event(const std::uint32_t controllerIndex, const std::uint64_t ordinal) {
            return {.controller = Controller(controllerIndex),
                    .tick = 4,
                    .ordinal = ordinal,
                    .kind = CharacterFastPathEventKind::HitWall,
                    .impactVelocity = {1, 0, 0}};
        }

        CharacterFastPathImpulse Impulse(const std::uint32_t index, const std::uint64_t sequence) {
            return {.body = Physics::BodyHandle{World(), {index, 1}}, .sequence = sequence, .point = {0, 0, 0}, .impulse = {1, 0, 0}};
        }
    }  // namespace

    TEST_CASE("Character fast-path preparation reserves every configured hot collection", "[physics][character][fast-path]") {
        CharacterFastPathStorage storage{Settings()};

        REQUIRE(storage.Commands().capacity() == 2);
        REQUIRE(storage.CommandScratchCapacity() == 2);
        REQUIRE(storage.Contacts().empty());
        REQUIRE(storage.Hits().empty());
        REQUIRE(storage.Events().empty());

        REQUIRE(storage.TryAllocateScratch(16, alignof(std::max_align_t)).has_value());
        REQUIRE(storage.TryAllocateScratch(48, alignof(std::max_align_t)).has_value());
        REQUIRE_FALSE(storage.TryAllocateScratch(1, alignof(std::max_align_t)).has_value());
        REQUIRE(storage.Snapshot().scratchBytesUsed == 64);
        REQUIRE(storage.Snapshot().scratchOverflowCount == 1);
    }

    TEST_CASE("Character fast-path overflow retains the canonical contact and hit subset", "[physics][character][fast-path][capacity]") {
        CharacterFastPathStorage storage{Settings()};

        REQUIRE(storage.TryAppendContact(Contact(1, 0.1F)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendContact(Contact(2, 0.2F)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendContact(Contact(3, 1.0F)) == CharacterFastPathAppendStatus::ReplacedWorst);

        REQUIRE(storage.TryAppendHit(Hit(1, 4.0F)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendHit(Hit(2, 3.0F)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendHit(Hit(3, 1.0F)) == CharacterFastPathAppendStatus::ReplacedWorst);
        storage.Canonicalize();

        REQUIRE(storage.Contacts().size() == 2);
        REQUIRE(storage.Contacts()[0].penetrationDepthMeters == 1.0F);
        REQUIRE(storage.Hits().size() == 2);
        REQUIRE(storage.Hits()[0].distanceMeters == 1.0F);
        REQUIRE(storage.Snapshot().contactOverflowCount == 1);
        REQUIRE(storage.Snapshot().hitOverflowCount == 1);

        auto malformed = Contact(4, 0.5F);
        malformed.shape = {};
        REQUIRE(storage.TryAppendContact(malformed) == CharacterFastPathAppendStatus::RejectedInvalid);
        REQUIRE(storage.Snapshot().invalidInputCount == 1);
    }

    TEST_CASE("Character fast-path events and impulses reduce without allocation or invalid state", "[physics][character][fast-path]") {
        CharacterFastPathStorage storage{Settings()};
        REQUIRE(storage.TryAppendEvent(Event(1, 2)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendEvent(Event(1, 1)) == CharacterFastPathAppendStatus::Appended);
        auto malformed = Event(0, 1);
        malformed.controller = {};
        REQUIRE(storage.TryAppendEvent(malformed) == CharacterFastPathAppendStatus::RejectedInvalid);
        REQUIRE(storage.TryAppendImpulse(Impulse(1, 2)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendImpulse(Impulse(2, 1)) == CharacterFastPathAppendStatus::Appended);
        REQUIRE(storage.TryAppendImpulse(Impulse(1, 1)) == CharacterFastPathAppendStatus::ReplacedWorst);
        storage.Canonicalize();

        REQUIRE(storage.Events().size() == 2);
        REQUIRE(storage.Events()[0].ordinal == 1);
        REQUIRE(storage.Snapshot().stagedImpulses == 2);
        REQUIRE(storage.Snapshot().impulseOverflowCount == 1);

        const auto contact = Contact(1, 0.5F);
        const auto allocationsBefore = Tests::AllocationProbe::Count();
        storage.ResetTransient();
        REQUIRE(storage.TryAllocateScratch(8, alignof(std::max_align_t)).has_value());
        REQUIRE(storage.TryAppendContact(contact) == CharacterFastPathAppendStatus::Appended);
        storage.Canonicalize();
        const auto allocationsAfter = Tests::AllocationProbe::Count();
        REQUIRE(allocationsAfter == allocationsBefore);

        REQUIRE(storage.Snapshot().retainedContacts == 1);
        REQUIRE(storage.Snapshot().queuedEvents == 2);
        storage.ResetAll();
        REQUIRE(storage.Events().empty());
        REQUIRE(storage.Snapshot().queuedEvents == 0);
    }
}  // namespace Horo::Character::Detail
