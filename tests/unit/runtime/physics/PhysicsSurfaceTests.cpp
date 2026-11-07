#include "AllocationProbe.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsMaterialAsset.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "Horo/Physics/SurfaceMaterial.h"
#include "PhysicsEventProjection.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::Physics {
    namespace {
        template <class T> T SurfaceTestId(std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = value;
            return T::FromBytes(bytes);
        }

        PhysicsSurfaceWorldBinding SurfaceCapture() {
            auto catalog = SurfaceCatalogSnapshot::Create(
                {"proj_surfaces", 4, {{SurfaceTestId<SurfaceMaterialId>(1), "Stone"}, {SurfaceTestId<SurfaceMaterialId>(2), "Wood"}}});
            REQUIRE(catalog.HasValue());
            return {std::make_shared<const SurfaceCatalogSnapshot>(std::move(catalog).Value()), 7, SurfaceTestId<SurfaceMaterialId>(1)};
        }

        PhysicsQueryMaterial ActualPhysicalMaterial() {
            auto material = PhysicsMaterialAsset::Create(
                {.id = PhysicsMaterialAssetId::FromAssetId(SurfaceTestId<Assets::AssetId>(91)), .sourceRevision = 5});
            REQUIRE(material.HasValue());
            auto encoded = EncodePhysicsMaterialAsset(material.Value());
            REQUIRE(encoded.HasValue());
            auto decoded = DecodePhysicsMaterialAsset(encoded.Value());
            REQUIRE(decoded.HasValue());
            return {decoded.Value().Id().Asset(), decoded.Value().SourceRevision(), PhysicsMaterialSlotId::FromValue(1)};
        }

        PhysicsWorldSimulationBinding SurfaceSimulation(PhysicsSurfaceWorldBinding surfaces) {
            const auto layer = SurfaceTestId<CollisionLayerId>(3);
            const auto profile = SurfaceTestId<CollisionProfileId>(4);
            const auto channel = SurfaceTestId<PhysicsQueryChannelId>(5);
            ProjectCollisionSchema input{.defaultProfile = profile,
                                         .layers = {{layer}},
                                         .pairs = {{layer, layer, SimulationPairResponse::Block}},
                                         .queryChannels = {{channel}},
                                         .profiles = {{profile, layer, true, true, true, {{channel, CollisionQueryResponse::Block}}}}};
            auto schema = NormalizedCollisionSchema::Create(input);
            REQUIRE(schema.HasValue());
            return {std::make_shared<const NormalizedCollisionSchema>(std::move(schema).Value()), 1, std::move(surfaces)};
        }

        void SurfaceTick(PhysicsWorld &world, std::uint64_t tick) {
            REQUIRE(
                world.AdvanceFixedTick({.simulationTick = tick, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        }

        PhysicsQueryDescriptor SurfaceRay(PhysicsWorldId world, float x) {
            PhysicsQueryDescriptor request{.world = world, .sceneGeneration = 1, .geometry = PhysicsRayQuery{{x, 3, 0}, {0, -1, 0}, 10}};
            request.filter.channel = SurfaceTestId<PhysicsQueryChannelId>(5);
            return request;
        }
    }  // namespace

    TEST_CASE("Surface world identity includes explicit default and preserves "
              "immutable captured policy",
              "[physics-surface][identity]") {
        auto first = SurfaceCapture();
        auto second = first;
        second.defaultSurface = SurfaceTestId<SurfaceMaterialId>(2);
        CHECK(first.catalog->Fingerprint() == second.catalog->Fingerprint());
        CHECK(PhysicsSurfaceWorldFingerprint(first) != PhysicsSurfaceWorldFingerprint(second));
        auto changed = first;
        ++changed.mappingGeneration;
        const auto evidence = MakePhysicsSurfaceEvidence(first, *first.defaultSurface);
        CHECK(MatchesPhysicsSurfaceEvidence(first, evidence));
        CHECK_FALSE(MatchesPhysicsSurfaceEvidence(changed, evidence));
        auto foreign = SurfaceCatalogSnapshot::Create({"foreign", 4, {{*first.defaultSurface, "Stone"}}});
        REQUIRE(foreign.HasValue());
        changed = first;
        changed.catalog = std::make_shared<const SurfaceCatalogSnapshot>(std::move(foreign).Value());
        CHECK_FALSE(MatchesPhysicsSurfaceEvidence(changed, evidence));
        changed = first;
        changed.defaultSurface = SurfaceTestId<SurfaceMaterialId>(99);
        CHECK(ValidatePhysicsSurfaceWorldBinding(changed).HasError());
        static_assert(!std::is_convertible_v<Assets::AssetId, SurfaceMaterialId>);
    }

    TEST_CASE("Contradictory semantic callbacks reject the joined event candidate "
              "in either arrival order",
              "[physics-surface][events][failure]") {
        const auto capture = SurfaceCapture();
        const auto physical = ActualPhysicalMaterial();
        for (unsigned reverse = 0; reverse < 2; ++reverse) {
            Detail::PhysicsEventProjection projection(8, 8, PhysicsEventOverflowPolicy::DropNewest);
            const auto identity = PhysicsWorldId::Create(9634).Value();
            PhysicsContactObservation first{.simulationTick = 1,
                                            .first = {.body = {identity, {1, 1}},
                                                      .shape = {identity, {1, 1}},
                                                      .layer = SurfaceTestId<CollisionLayerId>(3),
                                                      .profile = SurfaceTestId<CollisionProfileId>(4),
                                                      .filterSchemaGeneration = 1},
                                            .second = {.body = {identity, {2, 1}},
                                                       .shape = {identity, {2, 1}},
                                                       .layer = SurfaceTestId<CollisionLayerId>(3),
                                                       .profile = SurfaceTestId<CollisionProfileId>(4),
                                                       .filterSchemaGeneration = 1},
                                            .firstMaterial = PhysicsEventMaterial{physical.asset, physical.assetGeneration, physical.slot},
                                            .contact = {.points = {PhysicsContactPoint{.normal = {0, 1, 0},
                                                                                       .penetrationDepthMeters = 0.02F}},
                                                        .pointCount = 1}};
            first.firstSurface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(1));
            projection.BeginTick(1);
            REQUIRE(projection.TryCapture(first));
            REQUIRE(projection.CompleteTick(1).HasValue());
            first.simulationTick = 2;
            auto second = first;
            second.firstSurface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(2));
            projection.BeginTick(2);
            REQUIRE(projection.TryCapture(reverse ? second : first));
            REQUIRE(projection.TryCapture(reverse ? first : second));
            auto failed = projection.CompleteTick(2);
            REQUIRE(failed.HasError());
            CHECK(failed.ErrorValue().code.Value() == SurfaceErrors::Invalid.code.Value());
            CHECK(projection.PublishedTick() == 1);
            REQUIRE(projection.PublishedEvents().size() == 1);
            CHECK(projection.PublishedEvents()[0].firstSurface == MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(1)));
            projection.AbortTick();
            projection.BeginTick(2);
            REQUIRE(projection.TryCapture(first));
            CHECK(projection.CompleteTick(2).HasValue());
        }
    }
#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Native compound queries copy two semantic surfaces for one real "
              "physical material without allocation",
              "[physics-surface][native][query]") {
        auto capture = SurfaceCapture();
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasValue());
        auto prepared = runtime.Value()->PrepareWorld(Test::SmallWorldSettings(), SurfaceSimulation(capture));
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        const auto identity = PhysicsWorldId::Create(9631).Value();
        REQUIRE(world->Activate(identity).HasValue());
        PhysicsCompoundShapeDescriptor compound;
        for (std::uint8_t i = 1; i <= 2; ++i) {
            PhysicsCompoundChild child{.geometry = PhysicsBoxShape{{0.5F, 0.5F, 0.5F}},
                                       .localPose = {{i == 1 ? -1.0F : 1.0F, 0, 0}, {}},
                                       .subshape = PhysicsShapeSubresourceId::FromValue(i),
                                       .material = ActualPhysicalMaterial(),
                                       .layer = SurfaceTestId<CollisionLayerId>(3),
                                       .profile = SurfaceTestId<CollisionProfileId>(4),
                                       .channel = SurfaceTestId<PhysicsQueryChannelId>(5)};
            child.surface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(i));
            compound.children.push_back(child);
        }
        PhysicsQueryFixtureDescriptor fixture{.shape = compound,
                                              .layer = SurfaceTestId<CollisionLayerId>(3),
                                              .profile = SurfaceTestId<CollisionProfileId>(4),
                                              .channel = SurfaceTestId<PhysicsQueryChannelId>(5)};
        auto admitted = world->CreateQueryFixture(fixture);
        REQUIRE(admitted.HasValue());
        SurfaceTick(*world, 1);
        std::array<PhysicsQueryHit, 1> hits;
        for (std::uint8_t i = 1; i <= 2; ++i) {
            const auto request = SurfaceRay(identity, i == 1 ? -1.0F : 1.0F);
            const auto before = Tests::AllocationProbe::Count();
            auto queried = world->Query(request, hits);
            const auto after = Tests::AllocationProbe::Count();
            REQUIRE(queried.HasValue());
            CHECK(before == after);
            REQUIRE(queried.Value().hitCount == 1);
            REQUIRE(hits[0].surface);
            CHECK(hits[0].surface->surface == SurfaceTestId<SurfaceMaterialId>(i));
            CHECK(hits[0].subshape == PhysicsShapeSubresourceId::FromValue(i));
            CHECK(hits[0].material->asset == ActualPhysicalMaterial().asset);
            CHECK(MatchesPhysicsSurfaceEvidence(capture, *hits[0].surface));
        }
        auto stale = compound;
        stale.children[0].surface->mappingGeneration++;
        CHECK(world->CreateQueryFixture({.shape = stale, .layer = fixture.layer, .profile = fixture.profile, .channel = fixture.channel})
                  .HasError());
        stale = compound;
        stale.children[0].surface->surface = SurfaceTestId<SurfaceMaterialId>(99);
        CHECK(world->CreateQueryFixture({.shape = stale, .layer = fixture.layer, .profile = fixture.profile, .channel = fixture.channel})
                  .HasError());
        const auto retained = *hits[0].surface;
        world->Shutdown();
        CHECK(MatchesPhysicsSurfaceEvidence(capture, retained));
        CHECK(world->Query(SurfaceRay(identity, 1), hits).HasError());
    }

    TEST_CASE("Authored resident compound evidence requires exact construction poses and distinct collider slots",
              "[physics-surface][native][compound]") {
        const auto capture = SurfaceCapture();
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasValue());
        auto prepared = runtime.Value()->PrepareWorld(Test::SmallWorldSettings(), SurfaceSimulation(capture));
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        const auto identity = PhysicsWorldId::Create(9635).Value();
        REQUIRE(world->Activate(identity).HasValue());
        auto shape = world->CreateSceneShape(PhysicsBoxShape{{0.5F, 0.25F, 0.5F}});
        REQUIRE(shape.HasValue());
        const std::array children{PhysicsSceneShapeInstance{shape.Value(), {{-2, 0, 0}, Math::Quaternion::Identity()}},
                                  PhysicsSceneShapeInstance{shape.Value(), {{2, 0, 0}, Math::Quaternion::Identity()}}};
        auto compound = world->CreateSceneCompoundShape(children);
        REQUIRE(compound.HasValue());
        PhysicsSceneBodyDescriptor descriptor;
        descriptor.body.shape = compound.Value();
        descriptor.collision = PhysicsSceneCollisionBinding{.profile = SurfaceTestId<CollisionProfileId>(4)};
        for (std::size_t index = 0; index < children.size(); ++index) {
            PhysicsSceneColliderBinding collider;
            collider.localPose = children[index].localPose;
            collider.subshape = PhysicsShapeSubresourceId::FromValue(static_cast<std::uint32_t>(21 + index));
            collider.materials = {ActualPhysicalMaterial()};
            collider.collider = 100 + index;
            collider.surface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(1 + index));
            collider.surface->collider = collider.collider;
            descriptor.collision->colliders.push_back(collider);
        }
        const auto publication = world->PublishedTick().publicationRevision;
        auto incorrectPose = descriptor;
        incorrectPose.collision->colliders[1].localPose = {};
        CHECK(world->CreateSceneBody(incorrectPose).HasError());
        auto duplicateSlot = descriptor;
        duplicateSlot.collision->colliders[1].collider = 100;
        duplicateSlot.collision->colliders[1].surface->collider = 100;
        CHECK(world->CreateSceneBody(duplicateSlot).HasError());
        CHECK(world->PublishedTick().publicationRevision == publication);
        auto body = world->CreateSceneBody(descriptor);
        REQUIRE(body.HasValue());
        SurfaceTick(*world, 1);
        std::array<PhysicsQueryHit, 1> hits;
        for (std::size_t index = 0; index < children.size(); ++index) {
            auto query = world->Query(SurfaceRay(identity, children[index].localPose.translation.x), hits);
            REQUIRE(query.HasValue());
            REQUIRE(query.Value().hitCount == 1);
            CHECK(hits[0].body == body.Value());
            CHECK(hits[0].subshape == descriptor.collision->colliders[index].subshape);
            REQUIRE(hits[0].material);
            const auto &expectedMaterial = descriptor.collision->colliders[index].materials.front();
            CHECK(hits[0].material->asset == expectedMaterial.asset);
            CHECK(hits[0].material->assetGeneration == expectedMaterial.assetGeneration);
            CHECK(hits[0].material->slot == expectedMaterial.slot);
            CHECK(hits[0].surface == descriptor.collision->colliders[index].surface);
            CHECK(hits[0].collider == descriptor.collision->colliders[index].collider);
        }
    }

    TEST_CASE("Same-world structural mapping replacement retires exact handles and "
              "old query publication fences",
              "[physics-surface][native][structural]") {
        const auto capture = SurfaceCapture();
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasValue());
        auto prepared = runtime.Value()->PrepareWorld(Test::SmallWorldSettings(), SurfaceSimulation(capture));
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        const auto identity = PhysicsWorldId::Create(9633).Value();
        REQUIRE(world->Activate(identity).HasValue());
        auto originalShape = world->CreateSceneShape(PhysicsBoxShape{{1, 0.25F, 1}});
        REQUIRE(originalShape.HasValue());
        PhysicsSceneColliderBinding collider;
        collider.collider = 100;
        collider.materials = {ActualPhysicalMaterial()};
        collider.surface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(1));
        collider.surface->collider = 100;
        PhysicsSceneBodyDescriptor original;
        original.body.shape = originalShape.Value();
        original.collision = PhysicsSceneCollisionBinding{.profile = SurfaceTestId<CollisionProfileId>(4), .colliders = {collider}};
        auto oldBody = world->CreateSceneBody(original);
        REQUIRE(oldBody.HasValue());
        auto capability = world->IssueQueryEventCapability();
        REQUIRE(capability.HasValue());
        SurfaceTick(*world, 1);
        const auto previous = world->PublishedTick();
        const PhysicsQueryCommand oldQuery{capability.Value().Identity(), previous.publicationRevision, SurfaceRay(identity, 0)};
        std::array<PhysicsQueryHit, 1> hits;
        REQUIRE(capability.Value().Submit(oldQuery, hits).HasValue());
        REQUIRE(hits[0].surface);
        const auto retained = hits[0];
        const std::array shapes{PhysicsSceneGroupShape{.geometry = PhysicsShapeDescriptor{PhysicsBoxShape{{1, 0.25F, 1}}}}};
        PhysicsSceneGroupBody body;
        body.shape = 0;
        body.descriptor = original;
        body.descriptor.collision->colliders[0].surface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(2));
        body.descriptor.collision->colliders[0].surface->collider = 100;
        auto group = world->PrepareSceneGroup(shapes, std::span{&body, 1});
        REQUIRE(group.HasValue());
        const auto replacement = group.Value()->Handles()[0];
        CHECK(replacement != oldBody.Value());
        CHECK(group.Value()->Shapes()[0] != originalShape.Value());
        const std::array retiredBodies{oldBody.Value()};
        const std::array retiredShapes{originalShape.Value()};
        REQUIRE(group.Value()->PrepareRetirement(retiredBodies, retiredShapes, {}).HasValue());
        REQUIRE(group.Value()->PrepareConstraints({}).HasValue());
        REQUIRE(group.Value()->ValidatePublication().HasValue());
        group.Value()->Publish();
        CHECK(world->PublishedTick().publicationRevision != previous.publicationRevision);
        CHECK(capability.Value().Submit(oldQuery, hits).HasError());
        SurfaceTick(*world, 2);
        const auto current = world->PublishedTick();
        const PhysicsQueryCommand next{capability.Value().Identity(), current.publicationRevision, SurfaceRay(identity, 0)};
        auto queried = capability.Value().Submit(next, hits);
        REQUIRE(queried.HasValue());
        REQUIRE(queried.Value().result.hitCount == 1);
        REQUIRE(hits[0].surface);
        CHECK(hits[0].body == replacement);
        CHECK(hits[0].body != retained.body);
        CHECK(hits[0].surface->surface == SurfaceTestId<SurfaceMaterialId>(2));
        CHECK(retained.surface->surface == SurfaceTestId<SurfaceMaterialId>(1));
        CHECK(MatchesPhysicsSurfaceEvidence(capture, *retained.surface));  // Historical association grants no live
                                                                           // query admission.
    }

    TEST_CASE("Native contacts retain each mapped collider endpoint for the same "
              "real physical asset",
              "[physics-surface][native][contact]") {
        const auto capture = SurfaceCapture();
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical);
        REQUIRE(runtime.HasValue());
        auto prepared = runtime.Value()->PrepareWorld(Test::SmallWorldSettings(), SurfaceSimulation(capture));
        REQUIRE(prepared.HasValue());
        auto world = std::move(prepared).Value();
        const auto identity = PhysicsWorldId::Create(9632).Value();
        REQUIRE(world->Activate(identity).HasValue());
        auto floor = world->CreateSceneShape(PhysicsBoxShape{{2, 0.25F, 2}});
        auto sphere = world->CreateSceneShape(PhysicsSphereShape{0.5F});
        REQUIRE(floor.HasValue());
        REQUIRE(sphere.HasValue());
        const std::array shapes{floor.Value(), sphere.Value()};
        for (std::uint8_t i = 1; i <= 2; ++i) {
            PhysicsSceneColliderBinding collider;
            collider.materials = {ActualPhysicalMaterial()};
            collider.collider = 100 + i;
            collider.surface = MakePhysicsSurfaceEvidence(capture, SurfaceTestId<SurfaceMaterialId>(i));
            collider.surface->collider = collider.collider;
            PhysicsSceneBodyDescriptor body;
            body.body.shape = shapes[i - 1];
            body.body.pose.translation = {0, i == 1 ? -0.25F : 0.45F, 0};
            if (i == 2) {
                body.body.motion = PhysicsMotionType::Dynamic;
                body.body.mass = PhysicsMass{1};
            }
            body.collision = PhysicsSceneCollisionBinding{.profile = SurfaceTestId<CollisionProfileId>(4), .colliders = {collider}};
            REQUIRE(world->CreateSceneBody(body).HasValue());
        }
        auto capability = world->IssueQueryEventCapability();
        REQUIRE(capability.HasValue());
        SurfaceTick(*world, 1);
        std::array<PhysicsEventRecord, 16> events;
        const auto tick = world->PublishedTick();
        auto read = capability.Value().ReadEvents({capability.Value().Identity(), tick.eventTick, tick.publicationRevision, 16}, events);
        REQUIRE(read.HasValue());
        REQUIRE(read.Value().recordCount != 0);
        const auto &event = events[0];
        REQUIRE(event.firstSurface);
        REQUIRE(event.secondSurface);
        REQUIRE(event.firstMaterial);
        REQUIRE(event.secondMaterial);
        CHECK(event.firstMaterial->asset == event.secondMaterial->asset);
        CHECK(event.firstSurface->surface != event.secondSurface->surface);
        CHECK(event.firstSurface->collider == event.pair.first.collider);
        CHECK(event.secondSurface->collider == event.pair.second.collider);
        CHECK(MatchesPhysicsSurfaceEvidence(capture, *event.firstSurface));
        CHECK(MatchesPhysicsSurfaceEvidence(capture, *event.secondSurface));
    }
#endif
}  // namespace Horo::Physics
