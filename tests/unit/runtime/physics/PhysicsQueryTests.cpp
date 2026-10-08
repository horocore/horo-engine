#include "AllocationProbe.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
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

        /** @brief Checks compound admission thread ownership without sharing native state across threads. */
        [[nodiscard]] bool RejectsFixtureOnForeignThread(PhysicsWorld &world, const PhysicsQueryFixtureDescriptor &fixture) {
            bool rejected = false;
            std::thread foreign([&] {
                const auto result = world.CreateQueryFixture(fixture);
                rejected = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
            });
            foreign.join();
            return rejected;
        }
    }  // namespace

    TEST_CASE("Inline capsule queries reject malformed axes dimensions depth and selector evidence", "[physics][query][capsule]") {
        auto descriptor = RayDescriptor();
        descriptor.geometry = PhysicsCapsuleSweepQuery{{0.25F, 0.5F}, {}, {0, 1, 0}, {1, 0, 0}, 1};
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());
        auto &sweep = std::get<PhysicsCapsuleSweepQuery>(descriptor.geometry);
        sweep.up = {0, 0, 0};
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasError());
        sweep.up = {0, 1, 0};
        sweep.maximumDistanceMeters = 0;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasError());
        descriptor.geometry = PhysicsCapsuleOverlapQuery{{-0.25F, 0.5F}, {}, {0, 1, 0}};
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasError());
        descriptor.geometry = PhysicsCapsuleOverlapQuery{{0.25F, 0.5F}, {}, {0, -1, 0}};
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());
        auto hit = Hit(0);
        hit.penetrationDepthMeters = -1;
        REQUIRE(ValidatePhysicsQueryHit(hit, descriptor).HasError());
        hit.penetrationDepthMeters = 0;
        descriptor.filter.blockingOnly = true;
        hit.response = PhysicsQueryResponse::Overlap;
        REQUIRE(ValidatePhysicsQueryHit(hit, descriptor).HasError());
        hit.response = PhysicsQueryResponse::Block;
        descriptor.filter.excludedBody = hit.body;
        REQUIRE(ValidatePhysicsQueryHit(hit, descriptor).HasError());
    }

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

    TEST_CASE("Analytic capsule overlap rejects malformed geometry before native construction", "[physics][query][descriptor]") {
        auto descriptor = RayDescriptor();
        PhysicsCapsuleOverlapQuery capsule{{0.5F, 0.5F}, {1, 2, 3}, {1, 0, 0}};
        descriptor.geometry = capsule;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasValue());
        SECTION("zero radius") {
            capsule.capsule.radiusMeters = 0;
        }
        SECTION("zero cylindrical height") {
            capsule.capsule.cylindricalHalfHeightMeters = 0;
        }
        SECTION("non-finite height") {
            capsule.capsule.cylindricalHalfHeightMeters = std::numeric_limits<float>::infinity();
        }
        SECTION("invalid up") {
            capsule.up = {0, 2, 0};
        }
        SECTION("non-finite position") {
            capsule.position.z = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("outside origin envelope") {
            capsule.position.y = MaximumPhysicsLocalHalfExtentMeters + 1;
        }
        SECTION("outside shape envelope") {
            capsule.capsule.radiusMeters = MaximumPhysicsLocalHalfExtentMeters;
        }
        descriptor.geometry = capsule;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, World(), 9).HasError());
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

    TEST_CASE("Compound fixture validation protects child identity and metadata", "[physics][query][compound]") {
        PhysicsQueryFixtureDescriptor fixture;
        fixture.layer = CollisionLayerId::FromBytes(LayerBytes);
        fixture.profile = CollisionProfileId::FromBytes(ProfileBytes);
        fixture.channel = PhysicsQueryChannelId::FromBytes(ChannelBytes);
        PhysicsCompoundChild child{.geometry = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                   .subshape = PhysicsShapeSubresourceId::FromValue(7),
                                   .layer = fixture.layer,
                                   .profile = fixture.profile,
                                   .channel = fixture.channel};
        fixture.shape = PhysicsCompoundShapeDescriptor{{child}};
        REQUIRE(ValidatePhysicsQueryFixtureDescriptor(fixture, World()).HasValue());
        auto &children = std::get<PhysicsCompoundShapeDescriptor>(fixture.shape).children;
        children.front().subshape = {};
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        children.front().subshape = PhysicsShapeSubresourceId::FromValue(7);
        children.push_back(child);
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        children.back().subshape = PhysicsShapeSubresourceId::FromValue(8);
        children.back().localPose.translation.x = std::numeric_limits<float>::quiet_NaN();
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        children.back().localPose.translation.x = 1.0F;
        children.back().trigger = true;
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::OperationUnsupported);
        children.back().trigger = false;
        children.back().material = PhysicsQueryMaterial{};
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        children.back().material.reset();
        children.back().geometry = PhysicsStaticPlaneShape{};
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::OperationUnsupported);
        children.back().geometry = PhysicsBoxShape{};
        REQUIRE(ValidatePhysicsQueryFixtureDescriptor(fixture, World()).HasValue());
        children.back().layer = {};
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        children.back().layer = fixture.layer;
        children.back().response = static_cast<PhysicsQueryFixtureResponse>(255);
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::OperationUnsupported);
        children.back().response = PhysicsQueryFixtureResponse::Block;
        fixture.material = PhysicsQueryMaterial{};
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
        fixture.material.reset();
        children.resize(257, child);
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::CapacityExceeded);
        children.clear();
        RequireCode(ValidatePhysicsQueryFixtureDescriptor(fixture, World()), PhysicsErrors::DescriptorInvalid);
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

    TEST_CASE("Canonical compound queries return stable child metadata through local transforms", "[physics][query][compound][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(704).Value();
        REQUIRE(world->Activate(identity).HasValue());

        auto fixture = QueryFixture({0, 0, 0});
        fixture.subshape.reset();
        const auto asset = Assets::AssetId::Parse("b972cfcb-5cce-4c32-b3b5-a9c238057b83").Value();
        const PhysicsCompoundChild first{.geometry = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                         .localPose = {.translation = {0, 0, -5}},
                                         .subshape = PhysicsShapeSubresourceId::FromValue(101),
                                         .material = PhysicsQueryMaterial{asset, 3, PhysicsMaterialSlotId::FromValue(7)},
                                         .layer = fixture.layer,
                                         .profile = fixture.profile,
                                         .channel = fixture.channel};
        PhysicsCompoundChild second = first;
        second.localPose.translation.z = -10;
        second.subshape = PhysicsShapeSubresourceId::FromValue(42);
        second.profile = CollisionProfileId::FromBytes(OtherProfileBytes);
        second.material = PhysicsQueryMaterial{asset, 4, PhysicsMaterialSlotId::FromValue(8)};
        fixture.shape = PhysicsCompoundShapeDescriptor{{first, second}};
        const auto created = world->CreateQueryFixture(fixture);
        REQUIRE(created.HasValue());
        std::get<PhysicsCompoundShapeDescriptor>(fixture.shape).children[0].subshape = PhysicsShapeSubresourceId::FromValue(999);
        REQUIRE(RejectsFixtureOnForeignThread(*world, fixture));
        AdvanceOneTick(*world);

        auto query = QueryDescriptor(identity, PhysicsRayQuery{{0, 0, 0}, {0, 0, -1}, 20}, PhysicsQueryCollection::All, 2);
        std::array<PhysicsQueryHit, 2> hits{};
        const auto all = world->Query(query, hits);
        REQUIRE(all.HasValue());
        REQUIRE(all.Value().hitCount == 2);
        REQUIRE(hits[0].subshape == first.subshape);
        REQUIRE(hits[0].material->slot == first.material->slot);
        REQUIRE(hits[1].subshape == second.subshape);
        REQUIRE(hits[1].profile == second.profile);
        REQUIRE(hits[1].material->assetGeneration == 4);

        query.filter.requiredProfile = second.profile;
        query.collection = PhysicsQueryCollection::Any;
        query.maximumHitCount = 1;
        const auto filtered = world->Query(query, hits);
        REQUIRE(filtered.HasValue());
        REQUIRE(filtered.Value().hitCount == 1);
        REQUIRE(hits[0].subshape == second.subshape);

        REQUIRE(world->DestroyQueryFixture(created.Value()).HasValue());
        RequireCode(world->DestroyQueryFixture(created.Value()), PhysicsErrors::HandleStale);
        world->Shutdown();
        RequireCode(world->CreateQueryFixture(fixture), PhysicsErrors::InvalidState);
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

    TEST_CASE("Canonical analytic capsule overlap honors arbitrary up and exact generation filters", "[physics][query][native]") {
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        const auto identity = PhysicsWorldId::Create(704).Value();
        REQUIRE(world->Activate(identity).HasValue());
        auto targetDescriptor = QueryFixture({1.2F, 0, 0});
        targetDescriptor.shape = PhysicsBoxShape{{0.1F, 0.1F, 0.1F}};
        const auto target = world->CreateQueryFixture(targetDescriptor).Value();
        AdvanceOneTick(*world);
        std::array<PhysicsQueryHit, 1> hits{};
        auto descriptor =
            QueryDescriptor(identity, PhysicsCapsuleOverlapQuery{{0.25F, 1.0F}, {}, {1, 0, 0}}, PhysicsQueryCollection::Any, 1);
        const auto before = Tests::AllocationProbe::Count();
        const auto result = world->Query(descriptor, hits);
        const auto after = Tests::AllocationProbe::Count();
        REQUIRE(result.HasValue());
        REQUIRE(after == before);
        REQUIRE(result.Value().hitCount == 1);
        REQUIRE(hits.front().body == target.body);
        descriptor.geometry = PhysicsCapsuleOverlapQuery{{0.25F, 1.0F}, {}, {0, 1, 0}};
        REQUIRE(world->Query(descriptor, hits).Value().hitCount == 0);
        descriptor.geometry = PhysicsCapsuleOverlapQuery{{0.25F, 1.0F}, {}, {1, 0, 0}};
        descriptor.filter.excludedBody = target.body;
        REQUIRE(world->Query(descriptor, hits).Value().hitCount == 0);
        descriptor.filter.excludedBody.reset();
        REQUIRE(world->DestroyQueryFixture(target).HasValue());
        REQUIRE(world->Query(descriptor, hits).Value().hitCount == 0);
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
