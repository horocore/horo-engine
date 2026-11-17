#include "Horo/AI/EnvironmentQueryNavigation.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#if HORO_TEST_NAVIGATION_NATIVE
#include "navigation/NavigationGroundedProviderFixture.h"
#endif

#include <limits>

namespace Horo::AI {
    namespace {
        using namespace Navigation;
        using namespace Navigation::CoordinatorTestSupport;

        [[nodiscard]] QueryGeneratorOutput SurfaceOutput(std::span<QueryGeneratedItem> items) {
            return {{QueryStageId::Create(1).Value(), QueryGeneratorId::Create(2).Value(), QueryProviderId::Create(3).Value(),
                     Source().snapshot.Value()},
                    items.size(),
                    items};
        }

        TEST_CASE("EQS surface generation counts invalid coordinates and rejects stale or truncated samples atomically",
                  "[unit][ai][navigation][eqs][generator]") {
            const NavigationQueryProvenance provenance{World(), Topology(), {}, 1};
            NavigationSamplePositionResult samples{{{.position = {-1, 0, 2}, .provenance = provenance},
                                                    {.position = {std::numeric_limits<float>::quiet_NaN(), 0, 0}, .provenance = provenance},
                                                    {.position = {1, 0, 0}, .provenance = provenance},
                                                    {.position = {-2, 0, 3}, .provenance = provenance}}};
            constexpr auto Maximum = std::numeric_limits<std::int64_t>::max();
            const auto origin = Math::WorldCoordinate64::FromMillimeters(Maximum, 10, 20);
            const QueryNavigationSurfaceSource input{samples, Source(), origin};
            const auto current = Current({});
            std::array<QueryGeneratedItem, 4> items{};
            const auto output = SurfaceOutput(items);
            const auto result = GenerateQueryNavigationSurface(input, current, output);
            REQUIRE(result.HasValue());
            CHECK(result.Value().itemCount == 2);
            CHECK(result.Value().invalidCoordinateCount == 2);
            CHECK(result.Value().provenance.revision == output.provenance.revision);
            CHECK(items[0] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(Maximum - 1000, 10, 2020)});
            CHECK(items[1] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(Maximum - 2000, 10, 3020)});
            items = {};
            const auto original = items;

            auto changed = current;
            ++changed.source.originRevision;
            SECTION("stale frame") {
                const auto stale = GenerateQueryNavigationSurface(input, changed, output);
                REQUIRE(stale.HasError());
                CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            }
            SECTION("mixed topology") {
                samples.samples.back().provenance.topology = {};
                const auto stale = GenerateQueryNavigationSurface(input, current, output);
                REQUIRE(stale.HasError());
                CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            }
            SECTION("bounded provider truncation") {
                samples.truncated = true;
                CHECK(GenerateQueryNavigationSurface(input, current, output).HasError());
            }
            SECTION("destination cap applies before scanning provider samples") {
                CHECK(GenerateQueryNavigationSurface(input, current, SurfaceOutput(std::span{items}.first(3))).HasError());
            }
            CHECK(items == original);
        }

        /** @brief Owns the provider and scheduler until every probe has released its request. */
        struct ProbeOwner final {
            JobSystem jobs{{.workerCount = 0}};
            NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
            NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, Limits())).Value()};

            explicit ProbeOwner(std::unique_ptr<INavigationQueryBackend> backend = MakeObservedNavigationBackend()) {
                Activate(world, std::move(backend));
            }

            ~ProbeOwner() {
                jobs.Shutdown(ShutdownPolicy::Drain);
            }

            void Complete(const std::span<const NavigationPathCaller> callers, const std::uint32_t count) {
                REQUIRE(coordinator.Dispatch(1) == count);
                jobs.Shutdown(ShutdownPolicy::Drain);
                REQUIRE(coordinator.Commit(Current(callers)) == count);
            }
        };

        TEST_CASE("EQS Navigation preserves unavailable provider errors and consumes no inline result", "[unit][ai][navigation][eqs]") {
            ProbeOwner owner;
            QueryNavigationProbe probe{owner.coordinator};
            const NavigationPathCaller callers[]{Caller()};
            const auto stage = QueryStageId::Create(1).Value();
            REQUIRE(probe.Start(owner.world, stage, Input(), 0).Value());
            REQUIRE_FALSE(probe.Poll(Current(callers)).Value().has_value());
            owner.Complete(callers, 1);
            const auto unavailable = probe.Poll(Current(callers));
            REQUIRE(unavailable.HasError());
            CHECK(unavailable.ErrorValue().code.Value() == NavigationErrors::NoNavigationData.code.Value());
            CHECK_FALSE(probe.Poll(Current(callers)).Value().has_value());
            CHECK(owner.coordinator.IsDrained());
        }

        TEST_CASE("EQS Navigation retires replaced callers and abandoned pending reservations", "[unit][ai][navigation][eqs]") {
            ProbeOwner owner;
            const auto stage = QueryStageId::Create(1).Value();
            {
                QueryNavigationProbe probe{owner.coordinator};
                auto input = Input();
                input.request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
                REQUIRE(probe.Start(owner.world, stage, input, 0).Value());
                const NavigationPathCaller replaced[]{Caller(1, 2)};
                const auto stale = probe.Poll(Current(replaced));
                REQUIRE(stale.HasError());
                CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
                CHECK_FALSE(probe.Poll(Current(replaced)).Value().has_value());
            }
            owner.coordinator.CollectRetiredAtSafePoint();
            CHECK(owner.coordinator.IsDrained());
            {
                QueryNavigationProbe probe{owner.coordinator};
                auto input = Input();
                input.request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
                REQUIRE(probe.Start(owner.world, stage, input, 0).Value());
            }
            owner.coordinator.CollectRetiredAtSafePoint();
            CHECK(owner.coordinator.IsDrained());
        }

        TEST_CASE("EQS Navigation capacity rejection admits no probe and can retry after owner retirement", "[unit][ai][navigation][eqs]") {
            ProbeOwner owner;
            std::array<NavRequestHandle, 4> occupied;
            for (auto &handle : occupied) {
                auto accepted = owner.coordinator.Submit(owner.world, Input(), 0);
                REQUIRE(accepted.HasValue());
                handle = accepted.Value();
            }
            QueryNavigationProbe probe{owner.coordinator};
            const auto stage = QueryStageId::Create(1).Value();
            const auto rejected = probe.Start(owner.world, stage, Input(), 0);
            REQUIRE(rejected.HasValue());
            CHECK_FALSE(rejected.Value());
            const NavigationPathCaller callers[]{Caller()};
            CHECK_FALSE(probe.Poll(Current(callers)).Value().has_value());
            for (const auto handle : occupied)
                REQUIRE(owner.coordinator.Abandon(handle));
            owner.coordinator.CollectRetiredAtSafePoint();
            REQUIRE(probe.Start(owner.world, stage, Input(), 0).Value());
            probe.Cancel();
            owner.coordinator.CollectRetiredAtSafePoint();
            CHECK(owner.coordinator.IsDrained());
        }

        TEST_CASE("EQS Navigation partial paths are incomparable and one failed provider leaves other results intact",
                  "[unit][ai][navigation][eqs]") {
            const auto stage = QueryStageId::Create(1).Value();
            SECTION("Partial evidence has no path cost") {
                ProbeOwner owner{std::make_unique<ControlledBackend>(nullptr, false, true)};
                QueryNavigationProbe probe{owner.coordinator};
                const NavigationPathCaller callers[]{Caller()};
                auto input = Input();
                input.request.coveragePolicy = NavigationPathCoveragePolicy::AllowPartial;
                REQUIRE(probe.Start(owner.world, stage, input, 0).Value());
                owner.Complete(callers, 1);
                const auto partial = probe.Poll(Current(callers));
                REQUIRE(partial.HasValue());
                REQUIRE(partial.Value().has_value());
                CHECK(partial.Value()->availability == QueryTestAvailability::Incomparable);
                CHECK_FALSE(partial.Value()->filter.has_value());
                CHECK_FALSE(partial.Value()->metric.has_value());
            }
            SECTION("Provider exception is isolated to the failed request") {
                ProbeOwner owner{std::make_unique<ControlledBackend>(nullptr, true)};
                QueryNavigationProbe failed{owner.coordinator}, healthy{owner.coordinator};
                const NavigationPathCaller callers[]{Caller(), Caller(2)};
                auto input = Input(callers[0]);
                input.request.start.x = 2;
                REQUIRE(failed.Start(owner.world, stage, input, 0).Value());
                input = Input(callers[1]);
                input.request.start.x = 3;
                REQUIRE(healthy.Start(owner.world, stage, input, 0).Value());
                owner.Complete(callers, 2);
                const auto failure = failed.Poll(Current(callers));
                REQUIRE(failure.HasError());
                CHECK(failure.ErrorValue().code.Value() == NavigationErrors::ProviderFailed.code.Value());
                CHECK(failure.ErrorValue().message == "controlled provider exception");
                const auto success = healthy.Poll(Current(callers));
                REQUIRE(success.HasValue());
                REQUIRE(success.Value().has_value());
                CHECK(success.Value()->filter == true);
            }
        }

#if HORO_TEST_NAVIGATION_NATIVE
        TEST_CASE("EQS builds canonical points from actual bounded Detour surface samples", "[unit][ai][navigation][eqs][generator]") {
            ProbeOwner owner{Navigation::TestSupport::MakeGroundedProvider()};
            const auto lease = owner.world.Acquire(World());
            REQUIRE(lease.HasValue());
            const auto sampled = lease.Value().Backend().SamplePosition({.world = World(),
                                                                         .topology = Topology(),
                                                                         .center = {5, 0, 5},
                                                                         .radiusMeters = 8,
                                                                         .requirement = {.query = NavigationQueryKind::SamplePosition,
                                                                                         .quality = NavigationQualityLevel::Balanced,
                                                                                         .limits = {.maximumNodeExpansions = 32,
                                                                                                    .maximumResultPoints = 8,
                                                                                                    .maximumSearchDistanceMeters = 16}}},
                                                                        lease.Value().Cancellation());
            REQUIRE(sampled.HasValue());
            REQUIRE_FALSE(sampled.Value().samples.empty());
            std::array<QueryGeneratedItem, 8> items{};
            const auto origin = Math::WorldCoordinate64::FromMillimeters(20'000, 0, -20'000);
            const auto generated = GenerateQueryNavigationSurface({sampled.Value(), Source(), origin}, Current({}), SurfaceOutput(items));
            REQUIRE(generated.HasValue());
            CHECK(generated.Value().itemCount == sampled.Value().samples.size());
            CHECK(generated.Value().invalidCoordinateCount == 0);
            for (std::size_t index = 0; index < generated.Value().itemCount; ++index) {
                const auto value = std::get<Math::WorldCoordinate64>(items[index]).Millimeters();
                CHECK(value[0] >= 20'000);
                CHECK(value[0] <= 30'000);
                CHECK(value[1] == 0);
                CHECK(value[2] >= -20'000);
                CHECK(value[2] <= -10'000);
            }
        }

        TEST_CASE("EQS Navigation reads real Detour cost only after owner publication and rejects replaced revisions",
                  "[unit][ai][navigation][eqs][provider]") {
            ProbeOwner owner{Navigation::TestSupport::MakeGroundedProvider()};
            QueryNavigationProbe first{owner.coordinator}, second{owner.coordinator};
            const NavigationPathCaller callers[]{Caller(), Caller(2)};
            const auto stage = QueryStageId::Create(1).Value();
            auto input = Input(callers[0]);
            input.request.start = {1, 0, 1};
            input.request.destination = {9, 0, 9};
            REQUIRE(first.Start(owner.world, stage, input, 0).Value());
            input.caller = callers[1];
            REQUIRE(second.Start(owner.world, stage, input, 0).Value());
            REQUIRE_FALSE(first.Poll(Current(callers)).Value().has_value());
            owner.Complete(callers, 2);
            const auto sample = first.Poll(Current(callers));
            REQUIRE(sample.HasValue());
            REQUIRE(sample.Value().has_value());
            CHECK(sample.Value()->stage == stage);
            CHECK(sample.Value()->filter == true);
            REQUIRE(sample.Value()->metric.has_value());
            CHECK(*sample.Value()->metric > 0);

            auto changed = Current(callers);
            ++changed.source.originRevision;
            const auto stale = second.Poll(changed);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            owner.coordinator.CollectRetiredAtSafePoint();
            CHECK(owner.coordinator.IsDrained());
        }
#endif
    }  // namespace
}  // namespace Horo::AI
