#include "../../runtime/physics/PhysicsTestUtils.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "ProjectNavigationLinkClearance.h"
#include "navigation/IncrementalBakeFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Application::Internal {
    namespace {
        /** @brief A projected pair in canonical metres with a distinct Physics origin. */
        [[nodiscard]] ProjectNavigationClearanceRequest Request() {
            const Sha256Digest fingerprint{.bytes = {1}};
            const auto surface = Navigation::SurfaceId::Create(1).Value();
            return {.bakeFingerprint = fingerprint,
                    .profile = Navigation::NavigationAgentProfileId::Create(2).Value(),
                    .start = {surface, {10, 0, 0}, 0.5F},
                    .end = {surface, {14, 0, 0}, 0.5F},
                    .channel = Physics::PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                    .radiusMeters = 0.25F,
                    .heightMeters = 1.6F};
        }
    }  // namespace

    TEST_CASE("Navigation link clearance fails closed for invalid capsules or missing Physics authority", "[navigation][physics]") {
        const Physics::PhysicsQueryEventCapability unavailable;
        const ProjectNavigationClearancePublication publication{7, 1, 1, {-10, 0, 0}};
        auto request = Request();
        request.heightMeters = 0.25F;
        Physics::Test::RequireError(MeasureProjectNavigationLinkClearance(unavailable, publication, request),
                                    Navigation::NavigationErrors::BakeInputInvalid);
        request = Request();
        request.end.position = request.start.position;
        Physics::Test::RequireError(MeasureProjectNavigationLinkClearance(unavailable, publication, request),
                                    Navigation::NavigationErrors::BakeInputInvalid);
        Physics::Test::RequireError(MeasureProjectNavigationLinkClearance(unavailable, publication, Request()),
                                    Physics::PhysicsErrors::CapabilityStale);
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        Physics::Test::RequireError(MeasureProjectNavigationLinkClearance(unavailable, publication, Request(), cancelled.Token()),
                                    Navigation::NavigationErrors::BakeInputCancelled);
    }

    TEST_CASE("Rebuild clearance evidence fences projected endpoints and requires both directions", "[navigation][physics][tile_rebuild]") {
        namespace Nav = Navigation;
        const Nav::TestSupport::IncrementalBakeFixture fixture;
        const auto input = fixture.Input();
        const auto tile = fixture.Tiles().front().key;
        const Nav::NavMeshOffMeshLink link{.start = {1, 0, 1}, .end = {4, 0, 1}, .radiusMeters = 1, .bidirectional = true};
        const ProjectNavigationClearanceSample
            forward{tile.scope, {input->Fingerprint(), tile.profile, {tile.surface, link.start}, {tile.surface, link.end}, 0.75F, 2}};
        const ProjectNavigationClearanceSample
            reverse{tile.scope, {input->Fingerprint(), tile.profile, {tile.surface, link.end}, {tile.surface, link.start}, 0.6F, 2}};
        Physics::Test::RequireError(ApplyProjectNavigationLinkClearance(*input, tile, link, std::span{&forward, 1}),
                                    Nav::NavigationErrors::OperationUnsupported);
        std::array rows{forward, reverse};
        const auto clear = ApplyProjectNavigationLinkClearance(*input, tile, link, rows);
        REQUIRE(clear.HasValue());
        CHECK(clear.Value().radiusMeters == 0.6F);
        rows.back().evidence.end.position.x += 0.01F;
        Physics::Test::RequireError(ApplyProjectNavigationLinkClearance(*input, tile, link, rows),
                                    Nav::NavigationErrors::OperationUnsupported);
        rows.back() = reverse;
        rows.front().evidence.bakeFingerprint = {};
        Physics::Test::RequireError(ApplyProjectNavigationLinkClearance(*input, tile, link, rows),
                                    Nav::NavigationErrors::OperationUnsupported);
    }

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Native Physics measures Navigation link endpoints and the continuous corridor in the exact publication",
              "[navigation][physics]") {
        using namespace Physics;
        auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
        auto world = runtime->PrepareWorld(Test::SmallWorldSettings()).Value();
        REQUIRE(world->Activate(PhysicsWorldId::Create(71).Value()).HasValue());
        const auto request = Request();
        auto blocking = false;
        SECTION("Clear endpoint pair and sweep") {
            CHECK_FALSE(blocking);
        }
        SECTION("A mid-corridor obstacle invalidates otherwise clear endpoints") {
            blocking = true;
            const auto collider =
                world->CreateQueryFixture({.shape = PhysicsBoxShape{{0.1F, 0.3F, 0.3F}},
                                           .pose = {.translation = {2, 0.8F, 0}, .rotation = Math::Quaternion::Identity()},
                                           .layer = CollisionLayerId::Parse("12345678-1234-4234-8234-123456789abc").Value(),
                                           .profile = CollisionProfileId::Parse("12345678-1234-4234-8234-123456789abd").Value(),
                                           .channel = request.channel});
            REQUIRE(collider.HasValue());
        }
        REQUIRE(world->AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        const auto access = world->IssueQueryEventCapability().Value();
        const auto tick = world->PublishedTick();
        auto publication = ProjectNavigationClearancePublication{7, tick.completedTick, tick.publicationRevision, {-10, 0, 0}};
        const auto measured = MeasureProjectNavigationLinkClearance(access, publication, request);
        REQUIRE(measured.HasValue());
        if (blocking) {
            CHECK_FALSE(measured.Value());
        } else {
            REQUIRE(measured.Value());
            CHECK(measured.Value()->bakeFingerprint == request.bakeFingerprint);
            CHECK(measured.Value()->start == request.start);
            CHECK(measured.Value()->end == request.end);
            CHECK(measured.Value()->radiusMeters == request.radiusMeters);
            CHECK(measured.Value()->heightMeters == request.heightMeters);
        }
        ++publication.revision;
        Test::RequireError(MeasureProjectNavigationLinkClearance(access, publication, request), PhysicsErrors::QuerySnapshotStale);
    }
#endif
}  // namespace Horo::Application::Internal
