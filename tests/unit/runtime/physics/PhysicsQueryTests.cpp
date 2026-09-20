#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        constexpr std::array<std::uint8_t, 16> LayerBytes{1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        constexpr std::array<std::uint8_t, 16> ProfileBytes{2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
        constexpr std::array<std::uint8_t, 16> OtherProfileBytes{4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4};
        constexpr std::array<std::uint8_t, 16> ChannelBytes{3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3};

        [[nodiscard]] PhysicsWorldId World() {
            return PhysicsWorldId::Create(17).Value();
        }

        [[nodiscard]] BodyHandle Body(const std::uint32_t index = 1, const std::uint32_t generation = 1) {
            return {World(), {index, generation}};
        }

        [[nodiscard]] ShapeHandle Shape(const std::uint32_t index = 2, const std::uint32_t generation = 1) {
            return {World(), {index, generation}};
        }

        [[nodiscard]] PhysicsQueryDescriptor RayDescriptor() {
            PhysicsQueryDescriptor descriptor;
            descriptor.world = World();
            descriptor.sceneGeneration = 9;
            descriptor.geometry = PhysicsRayQuery{{1, 2, 3}, {0, 0, -1}, 100};
            descriptor.filter.channel = PhysicsQueryChannelId::FromBytes(ChannelBytes);
            return descriptor;
        }

        [[nodiscard]] PhysicsQueryHit Hit(const float distance = 1) {
            return {.body = Body(),
                    .shape = Shape(),
                    .subshape = PhysicsShapeSubresourceId::FromValue(4),
                    .material = std::nullopt,
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(ProfileBytes),
                    .channel = PhysicsQueryChannelId::FromBytes(ChannelBytes),
                    .filterSchemaGeneration = 7,
                    .response = PhysicsQueryResponse::Block,
                    .position = {1, 0, 0},
                    .normal = Math::Vec3{0, 1, 0},
                    .distanceMeters = distance};
        }

        template <typename Value> void RequireCode(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Physics filter IDs preserve canonical UUIDs and remain non-interchangeable", "[physics][query][identity]") {
        const auto channel = PhysicsQueryChannelId::Parse("03000000-0000-0000-0000-000000000003");
        REQUIRE(channel.HasValue());
        REQUIRE(channel.Value().Bytes() == ChannelBytes);
        REQUIRE(channel.Value().ToString() == "03000000-0000-0000-0000-000000000003");
        REQUIRE_FALSE(PhysicsQueryChannelId{}.IsValid());
        REQUIRE_FALSE(PhysicsQueryChannelId::Parse("03000000-0000-0000-0000-00000000000A").HasValue());
        REQUIRE_FALSE(PhysicsQueryChannelId::Parse("03000000-0000-0000-0000-0000000000-3").HasValue());
        REQUIRE_FALSE(PhysicsQueryChannelId::Parse("00000000-0000-0000-0000-000000000000").HasValue());
        static_assert(!std::is_same_v<CollisionLayerId, CollisionProfileId>);
        static_assert(!std::is_convertible_v<CollisionLayerId, PhysicsQueryChannelId>);
    }

    TEST_CASE("Physics query descriptors validate all backend-neutral geometry alternatives", "[physics][query][descriptor]") {
        auto descriptor = RayDescriptor();
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());

        descriptor.geometry = PhysicsSweepQuery{Shape(), {{1, 2, 3}, {0, 0, 0, 1}}, {1, 0, 0}, 5};
        descriptor.collection = PhysicsQueryCollection::All;
        descriptor.maximumHitCount = 16;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());

        descriptor.geometry = PhysicsOverlapQuery{Shape(), {{1, 2, 3}, {0, 0, 0, 1}}};
        descriptor.collection = PhysicsQueryCollection::ThroughFirstBlock;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());

        descriptor.geometry = PhysicsPointQuery{{4, 5, 6}};
        descriptor.filter.requiredLayer = CollisionLayerId::FromBytes(LayerBytes);
        descriptor.filter.requiredProfile = CollisionProfileId::FromBytes(ProfileBytes);
        descriptor.filter.excludedBody = Body();
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());
    }

    TEST_CASE("Physics query descriptor validation rejects stale malformed and unbounded requests", "[physics][query][descriptor]") {
        auto descriptor = RayDescriptor();
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, PhysicsWorldId::Create(18).Value(), 9), PhysicsErrors::HandleWorldMismatch);
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 10), PhysicsErrors::QuerySnapshotStale);

        descriptor.filter.channel = {};
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::DescriptorInvalid);
        descriptor.filter.channel = PhysicsQueryChannelId::FromBytes(ChannelBytes);
        descriptor.maximumHitCount = MaximumPhysicsQueryHits + 1;
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::CapacityExceeded);
        descriptor.maximumHitCount = 2;
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::DescriptorInvalid);
        descriptor.maximumHitCount = 1;
        descriptor.ordering = static_cast<PhysicsQueryOrdering>(255);
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::OperationUnsupported);

        descriptor.ordering = PhysicsQueryOrdering::ClosestFirst;
        descriptor.geometry = PhysicsRayQuery{{}, {0, 0, 2}, 1};
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::DescriptorInvalid);
        descriptor.geometry = PhysicsPointQuery{{std::numeric_limits<float>::quiet_NaN(), 0, 0}};
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::DescriptorInvalid);
        descriptor.geometry = PhysicsOverlapQuery{{PhysicsWorldId::Create(18).Value(), {2, 1}}, {}};
        RequireCode(ValidatePhysicsQueryDescriptor(descriptor, World(), 9), PhysicsErrors::HandleWorldMismatch);
    }

    TEST_CASE("Physics query hits validate stable identity material and subshape evidence", "[physics][query][hit]") {
        const auto descriptor = RayDescriptor();
        auto hit = Hit();
        REQUIRE(ValidatePhysicsQueryHit(hit, descriptor).HasValue());

        hit.material = PhysicsQueryMaterial{Assets::AssetId::Parse("b972cfcb-5cce-4c32-b3b5-a9c238057b83").Value(), 3,
                                            PhysicsMaterialSlotId::FromValue(8)};
        REQUIRE(ValidatePhysicsQueryHit(hit, descriptor).HasValue());
        hit.material->assetGeneration = 0;
        RequireCode(ValidatePhysicsQueryHit(hit, descriptor), PhysicsErrors::DescriptorInvalid);
        hit.material.reset();
        hit.distanceMeters = 101;
        RequireCode(ValidatePhysicsQueryHit(hit, descriptor), PhysicsErrors::DescriptorInvalid);
        hit.distanceMeters = 1;
        hit.subshape = PhysicsShapeSubresourceId{};
        RequireCode(ValidatePhysicsQueryHit(hit, descriptor), PhysicsErrors::DescriptorInvalid);
        hit.subshape.reset();
        hit.filterSchemaGeneration = 0;
        RequireCode(ValidatePhysicsQueryHit(hit, descriptor), PhysicsErrors::QuerySnapshotStale);
        hit.filterSchemaGeneration = 7;
        hit.body.world = PhysicsWorldId::Create(18).Value();
        RequireCode(ValidatePhysicsQueryHit(hit, descriptor), PhysicsErrors::HandleWorldMismatch);
    }

    TEST_CASE("Physics query hit ordering ignores native traversal and uses stable public evidence", "[physics][query][ordering]") {
        std::array hits{Hit(4), Hit(1), Hit(1), Hit(1)};
        hits[2].response = PhysicsQueryResponse::Overlap;
        hits[3].body = Body(0, 1);
        std::ranges::sort(hits, PhysicsQueryHitLess);
        REQUIRE(hits[0].distanceMeters == 1);
        REQUIRE(hits[0].response == PhysicsQueryResponse::Block);
        REQUIRE(hits[0].body.slot.index == 0);
        REQUIRE(hits[1].response == PhysicsQueryResponse::Block);
        REQUIRE(hits[2].response == PhysicsQueryResponse::Overlap);
        REQUIRE(hits[3].distanceMeters == 4);
        REQUIRE_FALSE(PhysicsQueryHitLess(hits[1], hits[1]));
    }

    TEST_CASE("Physics query result metadata cannot exceed the admitted bound or omit schema evidence", "[physics][query][result]") {
        auto descriptor = RayDescriptor();
        REQUIRE(ValidatePhysicsQueryResult({.hitCount = 1, .filterSchemaGeneration = 7}, descriptor).HasValue());
        RequireCode(ValidatePhysicsQueryResult({.hitCount = 2, .filterSchemaGeneration = 7}, descriptor), PhysicsErrors::CapacityExceeded);
        RequireCode(ValidatePhysicsQueryResult({.hitCount = 1}, descriptor), PhysicsErrors::QuerySnapshotStale);
    }

#if HORO_TEST_PHYSICS_NATIVE
    namespace {
        [[nodiscard]] PhysicsQueryFixtureDescriptor QueryFixture(
            const Math::Vec3 translation, const PhysicsQueryFixtureResponse response = PhysicsQueryFixtureResponse::Block,
            const bool trigger = false, const std::array<std::uint8_t, 16> &profileBytes = ProfileBytes) {
            return {.shape = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                    .pose = {.translation = translation, .rotation = Math::Quaternion::Identity()},
                    .layer = CollisionLayerId::FromBytes(LayerBytes),
                    .profile = CollisionProfileId::FromBytes(profileBytes),
                    .channel = PhysicsQueryChannelId::FromBytes(ChannelBytes),
                    .response = response,
                    .trigger = trigger,
                    .subshape = PhysicsShapeSubresourceId::FromValue(11)};
        }

        [[nodiscard]] PhysicsQueryDescriptor QueryDescriptor(const PhysicsWorldId world, const PhysicsQueryGeometry geometry,
                                                             const PhysicsQueryCollection collection, const std::uint32_t maximumHitCount) {
            return {.world = world,
                    .sceneGeneration = 1,
                    .geometry = geometry,
                    .filter = {.channel = PhysicsQueryChannelId::FromBytes(ChannelBytes)},
                    .collection = collection,
                    .ordering = PhysicsQueryOrdering::ClosestFirst,
                    .maximumHitCount = maximumHitCount};
        }

        void AdvanceOneTick(PhysicsWorld &world) {
            REQUIRE(world.AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasValue());
        }

        struct QueryDuringStep final {
            PhysicsWorld *world{};
            PhysicsQueryDescriptor *descriptor{};
            bool rejectedInvalidState{};
        };

        void QueryDuringStepCallback(void *context, const PhysicsTickPhase phase, const std::uint64_t) noexcept {
            if (phase != PhysicsTickPhase::BroadPhase)
                return;
            auto &state = *static_cast<QueryDuringStep *>(context);
            std::array<PhysicsQueryHit, 1> hits{};
            const auto result = state.world->Query(*state.descriptor, hits);
            if (result.HasError())
                state.rejectedInvalidState = result.ErrorValue().code.Value() == PhysicsErrors::InvalidState.code.Value();
        }
    }  // namespace

    TEST_CASE("Canonical immediate queries return deterministic identities and collection semantics", "[physics][query][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(700).Value();
        REQUIRE(world->Activate(identity).HasValue());
        REQUIRE(runtime->Capability(PhysicsCapability::ImmediateQueries) == PhysicsCapabilitySupport::Available);

        const auto first = world->CreateQueryFixture(QueryFixture({0, 0, -5}, PhysicsQueryFixtureResponse::Overlap)).Value();
        const auto second = world->CreateQueryFixture(QueryFixture({0, 0, -10})).Value();
        REQUIRE(first.body.slot.generation != second.body.slot.generation);
        AdvanceOneTick(*world);

        const auto rayGeometry = PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 20};
        std::array<PhysicsQueryHit, 2> hits{};
        auto descriptor = QueryDescriptor(identity, rayGeometry, PhysicsQueryCollection::All, 2);
        const auto all = world->Query(descriptor, hits).Value();
        REQUIRE(all.hitCount == 2);
        REQUIRE_FALSE(all.truncated);
        REQUIRE(hits[0].body == first.body);
        REQUIRE(hits[1].body == second.body);
        REQUIRE(hits[0].distanceMeters < hits[1].distanceMeters);
        REQUIRE(hits[0].subshape.has_value());
        REQUIRE(hits[0].filterSchemaGeneration != 0);

        std::array<PhysicsQueryHit, 1> shortHits{};
        const auto truncated = world->Query(descriptor, shortHits).Value();
        REQUIRE(truncated.hitCount == 1);
        REQUIRE(truncated.truncated);

        descriptor.collection = PhysicsQueryCollection::Closest;
        descriptor.maximumHitCount = 1;
        const auto closest = world->Query(descriptor, hits).Value();
        REQUIRE(closest.hitCount == 1);
        REQUIRE(hits[0].body == first.body);

        descriptor.collection = PhysicsQueryCollection::Any;
        const auto any = world->Query(descriptor, hits).Value();
        REQUIRE(any.hitCount == 1);
        REQUIRE(hits[0].body == first.body);

        descriptor.collection = PhysicsQueryCollection::ThroughFirstBlock;
        descriptor.maximumHitCount = 2;
        const auto through = world->Query(descriptor, hits).Value();
        REQUIRE(through.hitCount == 2);
        REQUIRE(hits[0].response == PhysicsQueryResponse::Overlap);
        REQUIRE(hits[1].response == PhysicsQueryResponse::Block);

        REQUIRE(world->DestroyQueryFixture(first).HasValue());
        REQUIRE(world->Query(descriptor, hits).HasValue());
        REQUIRE(world->DestroyQueryFixture(second).HasValue());
    }

    TEST_CASE("Canonical immediate queries apply channel, selector, trigger and exclusion filters", "[physics][query][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(701).Value();
        REQUIRE(world->Activate(identity).HasValue());

        const auto included = world->CreateQueryFixture(QueryFixture({0, 0, -5})).Value();
        const auto wrongProfile =
            world->CreateQueryFixture(QueryFixture({0, 0, -10}, PhysicsQueryFixtureResponse::Block, false, OtherProfileBytes)).Value();
        const auto ignored = world->CreateQueryFixture(QueryFixture({0, 0, -15}, PhysicsQueryFixtureResponse::Ignore)).Value();
        const auto trigger = world->CreateQueryFixture(QueryFixture({0, 0, -20}, PhysicsQueryFixtureResponse::Block, true)).Value();
        AdvanceOneTick(*world);

        std::array<PhysicsQueryHit, 4> hits{};
        auto descriptor = QueryDescriptor(identity, PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 30}, PhysicsQueryCollection::All, 4);
        auto result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 2);
        REQUIRE(hits[0].body == included.body);

        descriptor.filter.requiredProfile = CollisionProfileId::FromBytes(ProfileBytes);
        descriptor.filter.excludedBody = included.body;
        result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 0);
        descriptor.filter.triggers = PhysicsQueryTriggerPolicy::Include;
        result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 1);
        REQUIRE(hits[0].body == trigger.body);
        REQUIRE(world->DestroyQueryFixture(wrongProfile).HasValue());
        REQUIRE(world->DestroyQueryFixture(ignored).HasValue());
        REQUIRE(world->DestroyQueryFixture(trigger).HasValue());
        REQUIRE(world->DestroyQueryFixture(included).HasValue());
    }

    TEST_CASE("Canonical immediate point overlap and sweep queries project stable hits", "[physics][query][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(703).Value();
        REQUIRE(world->Activate(identity).HasValue());
        const auto target = world->CreateQueryFixture(QueryFixture({0, 0, -5})).Value();
        const auto source = world->CreateQueryFixture(QueryFixture({0, 0, 25})).Value();
        AdvanceOneTick(*world);

        std::array<PhysicsQueryHit, 2> hits{};
        auto descriptor = QueryDescriptor(identity, PhysicsPointQuery{{0, 0, -5}}, PhysicsQueryCollection::Closest, 1);
        auto result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 1);
        REQUIRE(hits[0].body == target.body);
        REQUIRE(hits[0].distanceMeters == 0.0F);
        REQUIRE_FALSE(hits[0].normal.has_value());

        descriptor.geometry = PhysicsOverlapQuery{source.shape, {{0, 0, -5}, Math::Quaternion::Identity()}};
        result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 1);
        REQUIRE(hits[0].body == target.body);
        REQUIRE(hits[0].distanceMeters == 0.0F);
        REQUIRE(hits[0].normal.has_value());

        descriptor.geometry = PhysicsSweepQuery{source.shape, {{0, 0, 0}, Math::Quaternion::Identity()}, {0, 0, -1}, 20};
        result = world->Query(descriptor, hits).Value();
        REQUIRE(result.hitCount == 1);
        REQUIRE(hits[0].body == target.body);
        REQUIRE(hits[0].distanceMeters > 0.0F);
        REQUIRE(hits[0].distanceMeters < 20.0F);
        REQUIRE(hits[0].normal.has_value());

        REQUIRE(world->DestroyQueryFixture(source).HasValue());
        RequireCode(world->Query(descriptor, hits), PhysicsErrors::HandleStale);
        REQUIRE(world->DestroyQueryFixture(target).HasValue());
    }

    TEST_CASE("Canonical immediate queries reject stale, invalid and reentrant execution", "[physics][query][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(702).Value();
        REQUIRE(world->Activate(identity).HasValue());
        const auto fixture = world->CreateQueryFixture(QueryFixture({0, 0, -5})).Value();
        auto descriptor = QueryDescriptor(identity, PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 20}, PhysicsQueryCollection::Closest, 1);
        std::array<PhysicsQueryHit, 1> hits{};
        RequireCode(world->Query(descriptor, hits), PhysicsErrors::QuerySnapshotStale);

        AdvanceOneTick(*world);
        descriptor.geometry = PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, std::numeric_limits<float>::quiet_NaN()};
        RequireCode(world->Query(descriptor, hits), PhysicsErrors::DescriptorInvalid);
        descriptor.geometry = PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 0};
        RequireCode(world->Query(descriptor, hits), PhysicsErrors::DescriptorInvalid);
        descriptor.geometry = PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 20};

        QueryDuringStep state{world.get(), &descriptor};
        REQUIRE(world
                    ->AdvanceFixedTick({.simulationTick = 2,
                                        .sceneGeneration = 1,
                                        .fixedDelta = Duration::FromNanoseconds(16'666'667),
                                        .observer = {.context = &state, .phase = QueryDuringStepCallback}})
                    .HasValue());
        REQUIRE(state.rejectedInvalidState);
        REQUIRE(world->DestroyQueryFixture(fixture).HasValue());
    }
#endif
}  // namespace Horo::Physics
