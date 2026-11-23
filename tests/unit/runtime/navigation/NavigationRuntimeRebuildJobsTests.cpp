#include "Horo/Foundation/JobSystem.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Navigation/NavigationRuntimeRebuildJobs.h"
#include "navigation/IncrementalBakeFixture.h"
#include "navigation/NavigationRuntimeTestFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::RequireError;

        /** @brief Two disconnected triangles with deliberately reversible local polygon indices. */
        [[nodiscard]] NavigationTileBuildResult LinkedTopology(const NavMeshTileKey &key, const Math::Aabb &bounds, bool reversed) {
            const auto area = TestSupport::Id<NavigationAreaId>(1);
            const auto first = reversed ? 3U : 0U;
            const auto second = reversed ? 0U : 3U;
            return {.state = NavigationTileBuildState::Built,
                    .key = key,
                    .bounds = bounds,
                    .vertices = {{1, 0, 1}, {2, 0, 1}, {1, 0, 2}, {4, 0, 1}, {5, 0, 1}, {4, 0, 2}},
                    .polygons = {{{0, 3}, {0, 3}, area}, {{3, 3}, {3, 3}, area}},
                    .polygonVertexIndices = {first, first + 1, first + 2, second, second + 1, second + 2},
                    .polygonAdjacencies = std::vector<std::uint32_t>(6, NavMeshBoundaryAdjacency),
                    .provenance = {{.producer = TestSupport::Id<NavigationSourceProducerId>(1),
                                    .contribution = TestSupport::Id<NavigationSourceContributionId>(1),
                                    .revision = TestSupport::Id<NavigationSourceRevision>(1),
                                    .sourceDigest = TestSupport::Digest(7),
                                    .polygons = {0, 2}}}};
        }

        class LinkedTileBuilder final : public INavigationMeshBuilder {
        public:
            [[nodiscard]] Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                                      const CancellationToken &) const override {
                return Result<NavigationTileBuildResult>::Success(LinkedTopology(request.key, request.bounds, true));
            }
        };

        class ObservedLinkProjector final : public INavigationRuntimeLinkProjector {
        public:
            bool changeAuthority{};
            mutable std::size_t calls{};

            [[nodiscard]] Result<NavMeshOffMeshLink> Project(const NavigationBakeInputSnapshot &input, const NavigationBakeTileKey &tile,
                                                             const NavigationTileBuildResult &topology, const NavMeshOffMeshLink &previous,
                                                             std::size_t scratch, const CancellationToken &) const override {
                ++calls;
                if (tile.tile != topology.key || !topology.offMeshLinks.empty() || !input.Revisions().geometry.IsValid() || scratch == 0)
                    return Result<NavMeshOffMeshLink>::Failure(MakeError(NavigationErrors::BakeInputInvalid));
                auto projected = previous;
                std::swap(projected.startPolygon, projected.endPolygon);
                if (changeAuthority)
                    projected.authoredLink = TestSupport::Id<NavigationLinkId>(2);
                return Result<NavMeshOffMeshLink>::Success(projected);
            }
        };

        /** @brief Produces one portable polygon so worker completion includes detached provider construction. */
        class JobTileBuilder final : public INavigationMeshBuilder {
        public:
            mutable std::size_t calls{};

            [[nodiscard]] Result<NavigationTileBuildResult> BuildTile(const NavigationTileBuildRequest &request,
                                                                      const CancellationToken &) const override {
                ++calls;
                const auto &triangle = request.triangles.front();
                const auto &source = triangle.provenance;
                return Result<NavigationTileBuildResult>::Success(
                    {.state = NavigationTileBuildState::Built,
                     .key = request.key,
                     .bounds = request.bounds,
                     .vertices = {triangle.vertices.begin(), triangle.vertices.end()},
                     .polygons = {{{0, 3}, {0, 3}, triangle.area}},
                     .polygonVertexIndices = {0, 1, 2},
                     .polygonAdjacencies = {NavMeshBoundaryAdjacency, NavMeshBoundaryAdjacency, NavMeshBoundaryAdjacency},
                     .provenance = {{.kind = source.kind,
                                     .producer = source.producer,
                                     .contribution = source.contribution,
                                     .revision = source.revision,
                                     .sourceDigest = source.contentDigest,
                                     .polygons = {0, 1}}}});
            }
        };

        /** @brief Records exact provider preparation without introducing native query dependencies. */
        class JobBackendFactory final : public INavigationRuntimeRebuildBackendFactory {
        public:
            mutable std::size_t calls{};
            mutable NavigationWorldActivationDescriptor prepared;
            bool exceedBudget{};

            [[nodiscard]] Result<NavigationPreparedRuntimeRebuildBackend> Prepare(
                const NavigationWorldActivationDescriptor &activation, std::span<const std::shared_ptr<const NavigationCookedTile>>,
                const std::size_t maximumOwnedBytes, const CancellationToken &) const override {
                ++calls;
                prepared = activation;
                auto backend = TestSupport::MakeObservedNavigationBackend();
                const auto reservedBytes = maximumOwnedBytes + (exceedBudget ? 1U : 0U);
                return Result<NavigationPreparedRuntimeRebuildBackend>::SuccessInPlace(std::move(backend), reservedBytes);
            }
        };

        /** @brief Creates a complete one-tile stored predecessor and a changed owned request for queued worker execution. */
        [[nodiscard]] NavigationRuntimeOwnedRebuildRequest Request(const NavigationWorldActivationDescriptor &active) {
            TestSupport::IncrementalBakeFixture fixture;
            const auto layout = fixture.Tiles().front();
            const auto initial = fixture.Input();
            const auto prepared = PrepareNavigationBakeTile(*initial, layout, fixture.compatibility);
            REQUIRE(prepared.HasValue());
            auto stored = NavigationCookedTile::Create(prepared.Value(), {.key = layout.key.tile, .bounds = layout.bounds});
            REQUIRE(stored.HasValue());
            fixture.ExcludeBorder();
            auto changed = fixture.Input();
            return {.expected = {.activation = active, .sources = changed->Revisions(), .overlay = 1, .origin = 1, .worldChanges = 1},
                    .nextTopology = TestSupport::Topology(active.topology.Value() + 1),
                    .input = std::move(changed),
                    .layout = {layout},
                    .activeTiles = {std::move(stored).Value()},
                    .compatibility = fixture.compatibility};
        }

        /** @brief Starts with a linked authored tile whose geometry dependency is still current. */
        [[nodiscard]] NavigationRuntimeOwnedRebuildRequest LinkedRequest() {
            auto request = Request(TestSupport::Activation());
            const auto &layout = request.layout.front();
            const auto prepared = PrepareNavigationBakeTile(*request.input, layout, request.compatibility);
            REQUIRE(prepared.HasValue());
            auto topology = LinkedTopology(layout.key.tile, layout.bounds, false);
            topology.offMeshLinks.emplace_back(Math::Vec3{1, 0, 1}, Math::Vec3{4, 0, 1}, 0.5F, 0U, 1U, topology.polygons.front().area, true,
                                               TestSupport::Id<NavigationLinkId>(1));
            auto cooked = NavigationCookedTile::Create(prepared.Value(), std::move(topology));
            REQUIRE(cooked.HasValue());
            request.activeTiles.front() = std::move(cooked).Value();
            return request;
        }
    }  // namespace

    TEST_CASE("Runtime rebuild jobs reproject linked tiles and fence authored semantics", "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto factory = std::make_shared<JobBackendFactory>();
        NavigationRuntimeRebuildJobs jobs{workers, std::make_shared<LinkedTileBuilder>(), factory};
        auto request = LinkedRequest();
        auto projector = std::make_shared<ObservedLinkProjector>();
        SECTION("Preserves the link identity as local polygon indices change") {
            projector->changeAuthority = false;
        }
        SECTION("Rejects a replacement identity before provider construction") {
            projector->changeAuthority = true;
        }
        request.linkProjector = projector;
        const auto accepted = jobs.Start(std::move(request));
        REQUIRE(accepted.HasValue());
        workers.Shutdown(ShutdownPolicy::Drain);
        auto completed = jobs.TakeCompleted();
        CHECK(projector->calls == 1);
        if (projector->changeAuthority) {
            RequireError(completed, NavigationErrors::BakeInputInvalid);
            CHECK(factory->calls == 0);
            return;
        }
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value());
        CHECK(completed.Value()->RebuiltTileCount() == 1);
        const auto &links = completed.Value()->Tiles().front()->Topology().offMeshLinks;
        REQUIRE(links.size() == 1);
        CHECK(links.front().authoredLink == TestSupport::Id<NavigationLinkId>(1));
        CHECK(links.front().startPolygon == 1);
        CHECK(links.front().endPolygon == 0);
        CHECK(factory->calls == 1);
    }

    TEST_CASE("Navigation rebuild jobs charge owned evidence before detached preparation", "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto factory = std::make_shared<JobBackendFactory>();
        NavigationRuntimeRebuildJobs jobs{workers, std::make_shared<JobTileBuilder>(), factory};
        auto request = Request(TestSupport::Activation());
        const std::size_t total = request.maximumOwnedBytes;
        request.retainedInputBytes = total;
        RequireError(jobs.Start(request), NavigationErrors::CapacityExceeded);
        CHECK_FALSE(jobs.Busy());

        constexpr std::size_t measuredBytes = 1024U * 1024U;
        request.retainedInputBytes = measuredBytes;
        const auto started = jobs.Start(std::move(request));
        REQUIRE(started.HasValue());
        workers.Shutdown(ShutdownPolicy::Drain);
        const auto completed = jobs.TakeCompleted();
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value());
        CHECK(completed.Value()->ReservedBytes() == total - measuredBytes);
    }

    TEST_CASE("Navigation rebuild jobs own queued input and leave the provider detached until owner publication",
              "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto builder = std::make_shared<JobTileBuilder>();
        const auto factory = std::make_shared<JobBackendFactory>();
        NavigationRuntimeRebuildJobs jobs{workers, builder, factory};
        auto created = NavigationWorldLifecycle::Create(1);
        REQUIRE(created.HasValue());
        auto world = std::move(created).Value();
        const auto active = TestSupport::Activation();
        REQUIRE(world.Stage(active, TestSupport::MakeObservedNavigationBackend()).HasValue());
        REQUIRE(world.CommitAtSafePoint(active.scene, active.sceneGeneration).HasValue());

        auto request = Request(active);
        const auto fence = request.expected;
        auto successor = active;
        successor.topology = request.nextTopology;
        const std::weak_ptr<const NavigationBakeInputSnapshot> queuedInput{request.input};
        CHECK_FALSE(jobs.Busy());
        const auto started = jobs.Start(std::move(request));
        REQUIRE(started.HasValue());
        CHECK(jobs.Busy());
        CHECK_FALSE(queuedInput.expired());
        const auto pending = jobs.TakeCompleted();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value().has_value());
        RequireError(jobs.Start(Request(active)), NavigationErrors::AdmissionRejected);
        CHECK(builder->calls == 0);
        CHECK(factory->calls == 0);

        workers.Shutdown(ShutdownPolicy::Drain);
        CHECK(jobs.Busy());
        RequireError(jobs.Start(Request(active)), NavigationErrors::AdmissionRejected);
        CHECK(world.ActiveDescriptor().Value() == active);
        CHECK_FALSE(world.HasStagedCandidate());
        auto completed = jobs.TakeCompleted();
        REQUIRE(completed.HasValue());
        REQUIRE(completed.Value().has_value());
        CHECK_FALSE(jobs.Busy());
        CHECK(builder->calls == 1);
        CHECK(factory->calls == 1);
        CHECK(factory->prepared == successor);
        auto candidate = std::move(completed).Value();
        CHECK(candidate->RebuiltTileCount() == 1);
        CHECK(world.ActiveDescriptor().Value() == active);
        CHECK_FALSE(world.HasStagedCandidate());

        REQUIRE(std::move(*candidate).PublishAtSafePoint(world, fence).HasValue());
        CHECK(world.ActiveDescriptor().Value() == successor);
        CHECK_FALSE(jobs.TakeCompleted().Value().has_value());
    }

    TEST_CASE("Cancelling pending navigation rebuild jobs withdraws the candidate and releases the slot",
              "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto builder = std::make_shared<JobTileBuilder>();
        const auto factory = std::make_shared<JobBackendFactory>();
        NavigationRuntimeRebuildJobs jobs{workers, builder, factory};
        CancellationSource parent;
        REQUIRE(jobs.Start(Request(TestSupport::Activation()), parent.Token()).HasValue());
        REQUIRE(jobs.Busy());

        SECTION("The owner cancels pending work") {
            jobs.Cancel();
        }
        SECTION("The parent token cancels pending work") {
            parent.RequestCancellation();
        }

        workers.Shutdown(ShutdownPolicy::Drain);
        RequireError(jobs.TakeCompleted(), NavigationErrors::BakeInputCancelled);
        CHECK_FALSE(jobs.Busy());
        CHECK(builder->calls == 0);
        CHECK(factory->calls == 0);
        CHECK_FALSE(jobs.TakeCompleted().Value().has_value());
    }

    TEST_CASE("Cancellation withdraws a worker-completed navigation rebuild before the owner takes it",
              "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto builder = std::make_shared<JobTileBuilder>();
        const auto factory = std::make_shared<JobBackendFactory>();
        NavigationRuntimeRebuildJobs jobs{workers, builder, factory};
        REQUIRE(jobs.Start(Request(TestSupport::Activation())).HasValue());
        workers.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(factory->calls == 1);
        REQUIRE(jobs.Busy());
        jobs.Cancel();
        RequireError(jobs.TakeCompleted(), NavigationErrors::BakeInputCancelled);
        CHECK_FALSE(jobs.Busy());
    }

    TEST_CASE("A navigation rebuild job rejects excess provider storage before transferring a candidate",
              "[unit][navigation][tile_rebuild][jobs]") {
        JobSystem workers{{.workerCount = 0}};
        const auto builder = std::make_shared<JobTileBuilder>();
        const auto factory = std::make_shared<JobBackendFactory>();
        factory->exceedBudget = true;
        NavigationRuntimeRebuildJobs jobs{workers, builder, factory};
        REQUIRE(jobs.Start(Request(TestSupport::Activation())).HasValue());
        workers.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(factory->calls == 1);
        RequireError(jobs.TakeCompleted(), NavigationErrors::CapacityExceeded);
        CHECK_FALSE(jobs.Busy());
        CHECK_FALSE(jobs.TakeCompleted().Value().has_value());
    }
}  // namespace Horo::Navigation
