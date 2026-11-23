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
    }  // namespace

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
