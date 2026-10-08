#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/ReplicationRegistration.h"
#include "Horo/Network/NetworkErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Network;

    ComponentDescriptor Component(const std::string_view id = "game.tests.health") {
        return {.typeId = ComponentTypeId::Parse(id).Value(), .schemaVersion = 1, .displayName = "Health", .category = "Tests"};
    }

    GameplayReplicationRegistration Registration(const GameplayReplicationOwner &owner, const std::uint64_t schemaValue = 1) {
        const auto valueType = ReplicationValueTypeId::Create(1).Value();
        const auto codec = ReplicationCodecId::Create(1).Value();
        const ComponentTypeId component = ComponentTypeId::Parse("game.tests.health").Value();
        return {
            .owner = owner,
            .schema =
                {
                    .id = ReplicationSchemaId::Create(schemaValue).Value(),
                    .version = {1, 0},
                    .compatibility = {{1, 0}, {1, 0}},
                    .owner = {.value = "game.tests"},
                    .fields = {{.id = FieldId::Create(1).Value(),
                                .valueType = valueType,
                                .codec = codec,
                                .introducedVersion = {1, 0},
                                .limits = {8, 1}}},
                },
            .serializers = {CanonicalScalarReplicationSerializer::Create({.valueType = valueType,
                                                                          .codec = codec,
                                                                          .owner = {.value = "game.tests"},
                                                                          .valueKind = ReplicationValueKind::FloatingPoint,
                                                                          .maximumEncodedBytes = 8,
                                                                          .maximumElementCount = 1})
                                .Value()},
            .schedule =
                {
                    .captureAccess = {.reads = {component}},
                    .applyAccess = {.writes = {component}},
                },
        };
    }

    template <typename T> void RequireGameplayError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE_FALSE(result.HasValue());
        REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("native component replication publishes exact Network registries", "[unit][gameplay][replication]") {
    const std::array components{Component()};
    ReplicationRegistrationRegistry registry{"game.tests"};
    REQUIRE(registry.Register(Registration(components.front().typeId)).HasValue());
    REQUIRE(registry.Freeze(components, {}, {}).HasValue());
    REQUIRE(registry.IsFrozen());

    auto lease = registry.Acquire();
    REQUIRE(lease.HasValue());
    REQUIRE(lease.Value().IsValid());
    REQUIRE(lease.Value().Registrations().size() == 1);
    CHECK(lease.Value().Registrations().front().schema == ReplicationSchemaId::Create(1).Value());
    CHECK(std::get<ComponentTypeId>(lease.Value().Registrations().front().owner) == components.front().typeId);
    CHECK(lease.Value().Descriptors()->Schemas().front().compatibility.minimum == ReplicationSchemaVersion{1, 0});
    CHECK(lease.Value().Descriptors()->Schemas().front().compatibility.maximum == ReplicationSchemaVersion{1, 0});
    const auto encoded = lease.Value().Serializers().Encode(ReplicationSchemaId::Create(1).Value(), FieldId::Create(1).Value(), 4.5);
    REQUIRE(encoded.HasValue());
}

TEST_CASE("native replication resolves component behavior and service owners", "[unit][gameplay][replication]") {
    const std::array components{Component()};
    BehaviorRegistration behavior;
    behavior.descriptor.typeId = BehaviorTypeId::Parse("game.tests.controller").Value();
    GameplayServiceRegistration service;
    service.descriptor.id = GameplayServiceId::Parse("game.tests.session").Value();

    SECTION("behavior") {
        ReplicationRegistrationRegistry registry{"game.tests"};
        REQUIRE(registry.Register(Registration(behavior.descriptor.typeId)).HasValue());
        REQUIRE(registry.Freeze(components, std::span<const BehaviorRegistration>{&behavior, 1}, {}).HasValue());
    }

    SECTION("service") {
        ReplicationRegistrationRegistry registry{"game.tests"};
        REQUIRE(registry.Register(Registration(service.descriptor.id)).HasValue());
        REQUIRE(registry.Freeze(components, {}, std::span<const GameplayServiceRegistration>{&service, 1}).HasValue());
    }
}

TEST_CASE("native replication fails closed for malformed ownership schedule and lifecycle", "[unit][gameplay][replication]") {
    const std::array components{Component()};

    ReplicationRegistrationRegistry missingOwner{"game.tests"};
    REQUIRE(missingOwner.Register(Registration(ComponentTypeId::Parse("game.tests.missing").Value())).HasValue());
    RequireGameplayError(missingOwner.Freeze(components, {}, {}), GameplayErrors::ReplicationOwnerMissing);

    auto invalidPhase = Registration(components.front().typeId);
    invalidPhase.schedule.capturePhase = GameplaySystemPhase::Presentation;
    ReplicationRegistrationRegistry phaseRegistry{"game.tests"};
    RequireGameplayError(phaseRegistry.Register(std::move(invalidPhase)), GameplayErrors::InvalidReplicationRegistration);

    auto foreign = Registration(components.front().typeId);
    foreign.schema.owner.value = "game.other";
    ReplicationRegistrationRegistry foreignRegistry{"game.tests"};
    RequireGameplayError(foreignRegistry.Register(std::move(foreign)), GameplayErrors::InvalidReplicationRegistration);

    ReplicationRegistrationRegistry duplicate{"game.tests"};
    REQUIRE(duplicate.Register(Registration(components.front().typeId)).HasValue());
    RequireGameplayError(duplicate.Register(Registration(components.front().typeId)), GameplayErrors::DuplicateReplicationSchema);
    REQUIRE(duplicate.Freeze(components, {}, {}).HasValue());
    RequireGameplayError(duplicate.Register(Registration(components.front().typeId)), GameplayErrors::ReplicationRegistryFrozen);

    auto missingCapture = Registration(components.front().typeId, 2);
    missingCapture.schedule.captureAccess.reads.clear();
    ReplicationRegistrationRegistry accessRegistry{"game.tests"};
    REQUIRE(accessRegistry.Register(std::move(missingCapture)).HasValue());
    RequireGameplayError(accessRegistry.Freeze(components, {}, {}), GameplayErrors::ReplicationOwnerMissing);

    auto nullSerializer = Registration(components.front().typeId, 3);
    nullSerializer.serializers.front().reset();
    ReplicationRegistrationRegistry serializerRegistry{"game.tests"};
    REQUIRE(serializerRegistry.Register(std::move(nullSerializer)).HasValue());
    REQUIRE(serializerRegistry.Freeze(components, {}, {}).HasError());
}

TEST_CASE("gameplay modules without replicated state freeze without publishing a generation", "[unit][gameplay][replication]") {
    ReplicationRegistrationRegistry registry{"game.tests"};
    REQUIRE(registry.Freeze({}, {}, {}).HasValue());
    REQUIRE(registry.IsFrozen());
    RequireGameplayError(registry.Acquire(), GameplayErrors::InvalidReplicationRegistration);
}

TEST_CASE("native registration order and component display rename preserve canonical declarations", "[unit][gameplay][replication]") {
    const std::array originalComponents{Component()};
    auto renamedComponents = originalComponents;
    renamedComponents.front().displayName = "Renamed presentation";
    renamedComponents.front().category = "Different editor category";
    ReplicationRegistrationRegistry original{"game.tests"};
    ReplicationRegistrationRegistry renamed{"game.tests"};
    const auto first = Registration(originalComponents.front().typeId, 1);
    auto second = Registration(originalComponents.front().typeId, 2);
    second.serializers = first.serializers;  // One exact codec adapter is shared by both schema contributions.
    REQUIRE(original.Register(second).HasValue());
    REQUIRE(original.Register(first).HasValue());
    REQUIRE(renamed.Register(first).HasValue());
    REQUIRE(renamed.Register(second).HasValue());
    REQUIRE(original.Freeze(originalComponents, {}, {}).HasValue());
    REQUIRE(renamed.Freeze(renamedComponents, {}, {}).HasValue());
    const auto before = original.Acquire().Value();
    const auto after = renamed.Acquire().Value();
    CHECK(before.Descriptors()->Fingerprint() == after.Descriptors()->Fingerprint());
    REQUIRE(after.Registrations().size() == 2);
    CHECK(after.Registrations().front().schema == ReplicationSchemaId::Create(1).Value());
    CHECK(after.Registrations().back().schema == ReplicationSchemaId::Create(2).Value());
    for (const auto &binding : after.Registrations()) {
        CHECK(after.Serializers().Encode(binding.schema, FieldId::Create(1).Value(), 4.5).Value() ==
              before.Serializers().Encode(binding.schema, FieldId::Create(1).Value(), 4.5).Value());
    }
}

TEST_CASE("native declaration qualification never publishes missing or conflicting codec code", "[unit][gameplay][replication]") {
    const std::array components{Component()};
    ReplicationRegistrationRegistry registry{"game.tests"};
    SECTION("declared field codec has no exact adapter") {
        auto declaration = Registration(components.front().typeId);
        declaration.schema.fields.front().codec = ReplicationCodecId::Create(2).Value();
        REQUIRE(registry.Register(std::move(declaration)).HasValue());
        RequireGameplayError(registry.Freeze(components, {}, {}), NetworkErrors::ReplicationSerializerUnknown);
    }
    SECTION("two schema contributions claim the same codec with different adapters") {
        REQUIRE(registry.Register(Registration(components.front().typeId, 1)).HasValue());
        REQUIRE(registry.Register(Registration(components.front().typeId, 2)).HasValue());
        RequireGameplayError(registry.Freeze(components, {}, {}), NetworkErrors::ReplicationSerializerConflict);
    }
    CHECK_FALSE(registry.IsFrozen());
    CHECK(registry.Acquire().HasError());
}
