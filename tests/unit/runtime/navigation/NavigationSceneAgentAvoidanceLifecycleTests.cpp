#include "AllocationProbe.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "navigation/NavigationSceneLifecycleFixture.h"

#include <limits>

namespace Horo::Navigation {
    namespace {
        using namespace SceneLifecycleTestSupport;

        NavigationCrowdSnapshot Capture(Fixture &fixture, const NavigationMovementObservation &current) {
            auto dynamic = std::move(NavigationDynamicRegistry::Create()).Value();
            REQUIRE(dynamic.CommitAtSafePoint(fixture.activation, current.path.tick).HasValue());
            const auto motion = fixture.lifecycle->CaptureMotion(*fixture.scene.ActiveScene(), current, {}, 1);
            REQUIRE(motion.HasValue());
            const std::array profiles{NavigationCrowdProfileFacts{.profile = NavigationAgentProfileId::Create(1).Value()}};
            auto captured = BuildNavigationCrowdSnapshot(fixture.registry.Snapshot().Value(), dynamic.Snapshot().Value(),
                                                         std::array{motion.Value()}, profiles, {}, current.path.tick);
            REQUIRE(captured.HasValue());
            return std::move(captured).Value();
        }

        NavigationVelocityWork Prepare(Fixture &fixture, const NavigationCrowdSnapshot &snapshot,
                                       const NavigationMovementObservation &current) {
            auto prepared = fixture.lifecycle->PrepareVelocity(*fixture.scene.ActiveScene(), current, snapshot,
                                                               {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = 0.1F});
            REQUIRE(prepared.HasValue());
            return prepared.Value();
        }

        /** @brief Own a deliberately late real solver job's immutable inputs and terminal result independently of Scene lifetime. */
        struct HeldAvoidanceWork final {
            std::shared_ptr<ProviderFence> fence{std::make_shared<ProviderFence>()};
            std::shared_ptr<std::optional<Result<NavigationAvoidanceOutcome>>> result{
                std::make_shared<std::optional<Result<NavigationAvoidanceOutcome>>>()};
            JobHandle job;

            HeldAvoidanceWork(JobSystem &jobs, const NavigationVelocityWork &work, const NavigationCrowdSnapshot &snapshot) {
                auto provider = CreateRecastDetourCrowdBackend();
                REQUIRE(provider.HasValue());
                auto solver = std::shared_ptr<INavigationCrowdBackend>{std::move(provider).Value()};
                fence->held = true;
                auto admitted = jobs.Submit({}, [work, snapshot, solver, fence = fence, result = result](const CancellationToken &) {
                    {
                        std::unique_lock lock{fence->mutex};
                        fence->entered = true;
                        fence->condition.notify_all();
                        if (!fence->condition.wait_for(lock, std::chrono::seconds{3}, [&] {
                            return !fence->held;
                        }))
                            return;
                    }
                    *result = solver->Solve(snapshot, work.request);
                });
                REQUIRE(admitted.HasValue());
                job = std::move(admitted).Value();
                REQUIRE(fence->WaitEntered());
            }

            ~HeldAvoidanceWork() {
                // Release even on an assertion exception; fixture teardown then owns the actual JobSystem join.
                fence->Release();
            }

            void Finish(JobSystem &jobs) {
                fence->Release();
                jobs.Shutdown(ShutdownPolicy::Drain);
                REQUIRE(result->has_value());
                REQUIRE(result->value().HasValue());
            }
        };
    }  // namespace

    TEST_CASE("Scene lifecycle fences late real avoidance work after teleport destroy unload and explicit cancellation",
              "[unit][navigation][scene][lifecycle][avoidance][jobs]") {
        for (int cause = 0; cause < 4; ++cause) {
            Fixture fixture{1};
            fixture.PublishRoute();
            const auto current = fixture.Now(1);
            const auto snapshot = Capture(fixture, current);
            const auto work = Prepare(fixture, snapshot, current);
            HeldAvoidanceWork late{fixture.jobs, work, snapshot};
            if (cause == 0) {
                auto teleported = current;
                teleported.position = {8, 0, 1};
                REQUIRE(fixture.lifecycle->TeleportAtSafePoint(*fixture.scene.ActiveScene(), teleported, 2).HasValue());
            } else if (cause == 1)
                fixture.DestroyEntity();
            else if (cause == 2) {
                REQUIRE(fixture.scene.QueueUnload().HasValue());
                fixture.CommitScene();
                REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasError());
            } else {
                REQUIRE(fixture.lifecycle->CancelVelocity(work));
                REQUIRE_FALSE(fixture.lifecycle->CancelVelocity(work));
            }
            late.Finish(fixture.jobs);
            if (const auto scene = fixture.scene.ActiveScene(); scene.has_value())
                REQUIRE(fixture.lifecycle->CommitVelocity(*scene, work, late.result->value().Value(), snapshot, current).HasError());
            else
                REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(std::nullopt).HasError());
            REQUIRE(fixture.coordinator.IsDrained());
        }
    }

    TEST_CASE("Scene lifecycle rejects out-of-range avoidance indices without narrowing to a live agent",
              "[unit][navigation][scene][lifecycle][avoidance]") {
        Fixture fixture{1};
        fixture.PublishRoute();
        const auto current = fixture.Now(1);
        const auto snapshot = Capture(fixture, current);
        NavigationAvoidanceRequest request{.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = 0.1F};
        const auto scene = *fixture.scene.ActiveScene();
        const auto rejected = fixture.lifecycle->PrepareVelocity(scene, current, snapshot, {.agentIndex = snapshot.Agents().size()});
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
        if constexpr (std::numeric_limits<std::size_t>::max() > std::numeric_limits<std::uint32_t>::max()) {
            request.agentIndex = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) + 1;
            const auto work = Prepare(fixture, snapshot, current);
            const auto oversized = fixture.lifecycle->PrepareVelocity(scene, current, snapshot, request);
            REQUIRE(oversized.HasError());
            REQUIRE(oversized.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            auto late = work;
            late.request.agentIndex = request.agentIndex;
            REQUIRE(fixture.lifecycle->CommitVelocity(scene, late, {}, snapshot, current).HasError());
            REQUIRE(fixture.lifecycle->CancelVelocity(work));
        }
    }

    TEST_CASE("Scene-qualified real avoidance capture publication and reset add no owner-thread C++ allocation",
              "[unit][navigation][scene][lifecycle][avoidance][allocation]") {
        Fixture fixture{1};
        fixture.PublishRoute();
        const auto current = fixture.Now(1);
        const auto snapshot = Capture(fixture, current);
        auto provider = CreateRecastDetourCrowdBackend();
        REQUIRE(provider.HasValue());
        const auto work = Prepare(fixture, snapshot, current);
        const auto solved = provider.Value()->Solve(snapshot, work.request);
        REQUIRE(solved.HasValue());
        Horo::Tests::AllocationProbe::Measurement measured;
        bool published = false;
        bool reset = false;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement observation;
            published = fixture.lifecycle->CommitVelocity(*fixture.scene.ActiveScene(), work, solved.Value(), snapshot, current).HasValue();
            auto teleported = fixture.Now(2);
            teleported.position = {8, 0, 1};
            reset = fixture.lifecycle->TeleportAtSafePoint(*fixture.scene.ActiveScene(), teleported, 2).HasValue();
            measured = observation.Snapshot();
        }
        REQUIRE(published);
        REQUIRE(reset);
        REQUIRE(measured.requests == 0);
    }
}  // namespace Horo::Navigation
