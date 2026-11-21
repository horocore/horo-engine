#include "EnvironmentQueryTestSupport.h"
#include "GuardCover.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryNavigation.h"
#include "Horo/Gameplay/TacticalCover.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"

#if HORO_TEST_NAVIGATION_NATIVE
#include "navigation/NavigationGroundedProviderFixture.h"
#endif
#if HORO_TEST_NAVIGATION_NATIVE && HORO_TEST_PHYSICS_NATIVE
#include "Horo/AI/EnvironmentQueryPhysics.h"
#include "PerceptionSightTestSupport.h"
#endif

#include <limits>
#include <thread>

namespace Horo::AI {
    namespace {
        using namespace Navigation;
        using namespace Navigation::CoordinatorTestSupport;
        using namespace Gameplay;

        [[nodiscard]] TacticalCoverPoint CoverPoint(const Runtime::EntityRef anchor, const std::uint64_t id = 1) {
            return {TacticalCoverId::Create(id).Value(),
                    anchor,
                    Math::WorldCoordinate64::FromMillimeters(9000, 0, 9000),
                    {0, 0, -1},
                    TacticalCoverStance::Crouched};
        }

        TEST_CASE("Gameplay cover reservations reject contention, validate receipts and survive unchanged publication",
                  "[unit][ai][navigation][eqs][cover]") {
            auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto first = *view.Find({1});
            const auto second = *view.Find({2});
            const std::array points{CoverPoint(second, 2), CoverPoint(first)};
            TacticalCoverRegistry cover;
            REQUIRE(cover.Publish(view, 1, points).HasValue());
            CHECK(cover.Capture(view, 1).HasError());
            const auto candidates = cover.Capture(view, 2);
            REQUIRE(candidates.HasValue());
            REQUIRE(candidates.Value().size() == 2);
            CHECK(candidates.Value()[0].point == points[1]);
            const auto selected = candidates.Value()[0].handle;
            const auto acquired = cover.TryReserve(view, selected, first);
            REQUIRE(acquired.HasValue());
            REQUIRE(acquired.Value().has_value());
            REQUIRE(cover.Publish(view, 2, points).HasValue());
            CHECK(cover.TryReserve(view, selected, first).Value() == acquired.Value());
            const auto contested = cover.TryReserve(view, selected, second);
            REQUIRE(contested.HasValue());
            CHECK_FALSE(contested.Value());

            auto invalid = points;
            invalid[0].direction = {};
            REQUIRE(cover.Publish(view, 3, invalid).HasError());
            REQUIRE(cover.Release(*acquired.Value()));
            const auto next = cover.TryReserve(view, selected, second);
            REQUIRE(next.HasValue());
            REQUIRE(next.Value().has_value());
            CHECK_FALSE(cover.Release(*acquired.Value()));
            CHECK(cover.TryReserve(view, selected, second).Value() == next.Value());
        }

        TEST_CASE("Replacement cover providers reject historical selections and receipts for the same live anchor",
                  "[unit][ai][navigation][eqs][cover]") {
            const auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto anchor = *view.Find({1});
            const auto occupant = *view.Find({2});
            const std::array points{CoverPoint(anchor)};

            TacticalCoverRegistry original;
            REQUIRE(original.Publish(view, 1, points).HasValue());
            const auto old = original.Capture(view, 1).Value().front().handle;
            const auto oldReceipt = original.TryReserve(view, old, occupant);
            REQUIRE(oldReceipt.HasValue());
            REQUIRE(oldReceipt.Value());

            TacticalCoverRegistry replacement;
            REQUIRE(replacement.Publish(view, 1, points).HasValue());
            const auto current = replacement.Capture(view, 1).Value().front().handle;
            CHECK(current.anchor == old.anchor);
            CHECK(current.revision == old.revision);
            CHECK(current.provider != old.provider);
            const auto stale = replacement.TryReserve(view, old, occupant);
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == AIErrors::EnvironmentQueryContextStale.code.Value());
            CHECK(replacement.Score(view, old, {}, {}, {}).HasError());
            CHECK(replacement.Resolve(view, old).HasError());
            REQUIRE(replacement.Resolve(view, current).HasValue());
            CHECK(replacement.Resolve(view, current).Value().point == points.front());

            const auto admitted = replacement.TryReserve(view, current, occupant);
            REQUIRE(admitted.HasValue());
            REQUIRE(admitted.Value());
            CHECK(admitted.Value()->sequence == oldReceipt.Value()->sequence);
            CHECK_FALSE(replacement.Release(*oldReceipt.Value()));
            CHECK_FALSE(replacement.TryReserve(view, current, anchor).Value());
            CHECK(replacement.Release(*admitted.Value()));
        }

        TEST_CASE("Cover registry rejects off-lane access before reading or changing the inventory", "[unit][ai][navigation][eqs][cover]") {
            const auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto anchor = *view.Find({1});
            const auto occupant = *view.Find({2});
            const std::array points{CoverPoint(anchor)};
            TacticalCoverRegistry cover;
            REQUIRE(cover.Publish(view, 1, points).HasValue());
            const auto selected = cover.Capture(view, 1).Value().front().handle;
            const auto receipt = cover.TryReserve(view, selected, occupant);
            REQUIRE(receipt.HasValue());
            REQUIRE(receipt.Value());

            std::array<bool, 6> rejected{};
            const auto offLane = [&](const auto &result) {
                return result.HasError() && result.ErrorValue().code.Value() == AIErrors::EnvironmentQueryContextInvalid.code.Value();
            };
            {
                const std::jthread worker{[&] {
                    rejected[0] = offLane(cover.Publish(view, 2, points));
                    rejected[1] = offLane(cover.Capture(view, 1));
                    rejected[2] = offLane(cover.Score(view, selected, {}, {}, {}));
                    rejected[3] = offLane(cover.TryReserve(view, selected, anchor));
                    rejected[4] = !cover.Release(*receipt.Value());
                    rejected[5] = offLane(cover.Resolve(view, selected));
                }};
            }
            CHECK(std::ranges::all_of(rejected, std::identity{}));
            CHECK(cover.TryReserve(view, selected, occupant).Value() == receipt.Value());
            CHECK(cover.Release(*receipt.Value()));
        }

        TEST_CASE("Cover selection rejects a retired anchor even before its inventory is republished",
                  "[unit][ai][navigation][eqs][cover]") {
            auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto oldAnchor = *view.Find({1});
            const auto occupant = *view.Find({2});
            const auto point = CoverPoint(oldAnchor);
            TacticalCoverRegistry cover;
            REQUIRE(cover.Publish(view, 1, std::span{&point, 1}).HasValue());
            const auto old = cover.Capture(view, 1);
            REQUIRE(old.HasValue());
            REQUIRE(old.Value().size() == 1);

            Runtime::SceneCommandBuffer unload;
            unload.Destroy(oldAnchor);
            REQUIRE(scene->Commit(unload).HasValue());
            REQUIRE(cover.Capture(scene->View(), 1).Value().empty());
            CHECK(cover.TryReserve(scene->View(), old.Value().front().handle, occupant).HasError());
            CHECK(cover.Score(scene->View(), old.Value().front().handle, {}, {}, {}).HasError());
            Runtime::SceneCommandBuffer reload;
            REQUIRE(reload.Create({}).IsValid());
            const auto committed = scene->Commit(reload);
            REQUIRE(committed.HasValue());
            REQUIRE(committed.Value().created.size() == 1);
            const auto replacement = CoverPoint(committed.Value().created[0].entity);
            CHECK(replacement.anchor != oldAnchor);
            REQUIRE(cover.Publish(scene->View(), 2, std::span{&replacement, 1}).HasValue());
            CHECK(cover.TryReserve(scene->View(), old.Value().front().handle, occupant).HasError());
            const auto newCover = cover.Capture(scene->View(), 1);
            REQUIRE(newCover.HasValue());
            REQUIRE(newCover.Value().size() == 1);
            CHECK(cover.TryReserve(scene->View(), newCover.Value().front().handle, occupant).Value().has_value());
        }

        TEST_CASE("Guard cover example breaks ties by authored identity and skips exposed or reserved points",
                  "[unit][ai][navigation][eqs][cover][example]") {
            const auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto first = *view.Find({1});
            const auto second = *view.Find({2});
            auto near = CoverPoint(first, 1);
            near.position = Math::WorldCoordinate64::FromMillimeters(8'000, 0, 9'000);
            const std::array points{CoverPoint(second, 2), near};
            TacticalCoverRegistry cover;
            REQUIRE(cover.Publish(view, 1, points).HasValue());
            const auto candidates = cover.Capture(view, points.size());
            REQUIRE(candidates.HasValue());
            REQUIRE(candidates.Value().size() == points.size());
            const auto route = QueryStageId::Create(1).Value();
            const auto sight = QueryStageId::Create(2).Value();
            const auto contention = QueryStageId::Create(3).Value();
            const QueryTestSample reachable{.stage = route, .filter = true, .metric = 3.0};
            const QueryTestSample hidden{.stage = sight, .filter = false};
            std::array evidence{Examples::GuardCoverObservation{candidates.Value()[1].handle, reachable, hidden},
                                Examples::GuardCoverObservation{candidates.Value()[0].handle, reachable, hidden}};

            const auto winner = Examples::TrySelectGuardCover(cover, view, first, evidence, contention);
            REQUIRE(winner.HasValue());
            REQUIRE(winner.Value());
            CHECK(winner.Value()->destination == near.position);
            const auto alternate = Examples::TrySelectGuardCover(cover, view, second, evidence, contention);
            REQUIRE(alternate.HasValue());
            REQUIRE(alternate.Value());
            CHECK(alternate.Value()->destination == points[0].position);
            REQUIRE(cover.Release(winner.Value()->reservation));
            evidence[1].visibility.filter = true;
            const auto empty = Examples::TrySelectGuardCover(cover, view, first, evidence, contention);
            REQUIRE(empty.HasValue());
            CHECK_FALSE(empty.Value());
        }

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

        /** @brief Composes the real query and Navigation reservation APIs against one retained scene authority. */
        class RetirementSource final : public IQueryExecutionSource {
        public:
            RetirementSource(ConsumerFixture &owner, Runtime::RuntimeSceneView scene)
                : owner_(owner), scene_(std::move(scene)), probe_(owner.coordinator) {}

            bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept override {
                return scene_.IsCurrent() && scene_.RuntimeId() == inputs.contexts->Scene();
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &, const QueryGeneratorOutput &output) override {
                const std::array points{Math::WorldCoordinate64::FromMillimeters(1'000, 2'000, 3'000)};
                return GenerateQueryPoints(points, output);
            }

            Result<bool> StartTest(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &) override {
                return probe_.Start(owner_.world, stage, Input(), 0);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                const std::array callers{Caller()};
                return probe_.Poll(Current(callers));
            }

            void CancelTest() noexcept override {
                probe_.Cancel();
            }

        private:
            ConsumerFixture &owner_;
            Runtime::RuntimeSceneView scene_;
            QueryNavigationProbe probe_;
        };

        /** @brief Builds one path-test plan with the normal immutable descriptor and context admission. */
        std::unique_ptr<QueryExecution> AdmitRetirementQuery(ConsumerFixture &owner, const Runtime::RuntimeSceneView &scene) {
            using namespace QueryTestSupport;
            const QueryFixture fixture;
            const QueryTestDescriptor test{MakeIdentity<QueryTestId>(17),
                                           fixture.generator.origin,
                                           fixture.items.front().id,
                                           {1, 1},
                                           {},
                                           {}};
            const auto registry = fixture.Registry(std::span{&test, 1});
            const auto plan = fixture.Plan(registry, test.id);
            const std::array rules{QueryScoreRule{.stage = plan.Stages()[1].id, .requiredFilterValue = true}};
            auto scoring = QueryScoringPlan::Capture(plan, registry, rules, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan, registry, providers.Value(), scene, QueryTestSupport::Source(scene));
            REQUIRE(contexts.HasValue());
            const QueryExecutionInputs inputs{{MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}}, contexts.Value()};
            const QueryGeneratorProvenance provenance{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1};
            auto result = QueryExecution::Admit(inputs, provenance, std::move(scoring).Value(), {0, 20, 1, 1, 1, false},
                                                std::make_unique<RetirementSource>(owner, scene));
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("EQS retires dispatched Navigation work across query destruction and scene lifetime changes",
                  "[unit][ai][navigation][eqs][qualification]") {
            ConsumerFixture owner{1, std::make_shared<Gate>()};
            Runtime::RuntimeSceneService scene;
            REQUIRE(scene.Startup({}).HasValue());
            REQUIRE(scene.QueuePreparation(QueryTestSupport::MakeSceneDefinition()).HasValue());
            const auto commitScene = [&] {
                REQUIRE(
                    scene
                        .OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, Runtime::FrameContext{1, {}, 0.0, 0, {}, false, {}})
                        .HasValue());
            };
            commitScene();
            REQUIRE(scene.ActiveScene());
            auto query = AdmitRetirementQuery(owner, *scene.ActiveScene());
            CHECK_FALSE(query->Sync(1));
            AiWorkBudget budget{8};
            query->Advance(1, {budget, budget, budget});
            REQUIRE(owner.coordinator.Dispatch(1) == 1);
            REQUIRE(owner.gate->AwaitEntry());
            CHECK_FALSE(owner.coordinator.IsDrained());

            SECTION("query retirement abandons the live request") {
                query.reset();
            }
            SECTION("committed scene unload makes the query stale") {
                REQUIRE(scene.QueueUnload().HasValue());
                commitScene();
                CHECK_FALSE(scene.ActiveScene());
            }
            SECTION("structural target retirement invalidates the captured scene revision") {
                Runtime::SceneCommandBuffer unload;
                const auto target = scene.ActiveScene()->Find({2});
                REQUIRE(target);
                unload.Destroy(*target);
                REQUIRE(scene.QueueStructuralCommands(std::move(unload)).HasValue());
                commitScene();
                REQUIRE(scene.ActiveScene());
                CHECK_FALSE(scene.ActiveScene()->Find({2}));
            }
            if (query) {
                REQUIRE(query->Sync(2));
                REQUIRE(query->Published());
                CHECK(query->Published()->status == QueryExecutionStatus::StaleInputs);
                CHECK(query->Published()->winners.empty());
            }

            REQUIRE(owner.world.Unload(World()).HasValue());
            CHECK(owner.world.RetiredCount() == 1);
            owner.FinishWorker();
            CHECK(owner.coordinator.Commit({.tick = 2}) == 0);
            CHECK(owner.coordinator.IsDrained());
            CHECK(owner.world.CollectRetired() == NavigationWorldLifecycleState::Empty);
            scene.Shutdown();
        }

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

        TEST_CASE("EQS Navigation rejects a completed path after its world retires", "[unit][ai][navigation][eqs]") {
            ProbeOwner owner;
            QueryNavigationProbe probe{owner.coordinator};
            const std::array callers{Caller()};
            REQUIRE(probe.Start(owner.world, QueryStageId::Create(1).Value(), Input(), 0).Value());
            owner.Complete(callers, 1);
            REQUIRE(owner.world.Unload(World()).HasValue());
            const auto retired = probe.Poll(Current(callers));
            REQUIRE(retired.HasError());
            CHECK(retired.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            CHECK(owner.coordinator.IsDrained());
        }

        TEST_CASE("EQS Navigation drops stale path access while a retired coordinator's worker still runs",
                  "[unit][ai][navigation][eqs][qualification]") {
            ConsumerFixture owner{1, std::make_shared<Gate>()};
            std::optional coordinator{NavigationCoordinator::Create(owner.jobs, Limits()).Value()};
            const auto stage = QueryStageId::Create(1).Value();
            const std::array callers{Caller()};
            QueryNavigationProbe probe{*coordinator};
            REQUIRE(probe.Start(owner.world, stage, Input(), 0).Value());
            REQUIRE(coordinator->Dispatch(1) == 1);
            REQUIRE(owner.gate->AwaitEntry());

            SECTION("shutdown withdraws results and lets the probe abandon its reservation") {
                coordinator->BeginShutdown();
            }
            SECTION("destruction clears the owner even while a worker retains coordinator state") {
                coordinator.reset();
            }
            const auto stale = probe.Poll(Current(callers));
            REQUIRE(stale.HasError());
            CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            const auto readmitted = probe.Start(owner.world, stage, Input(), 1);
            REQUIRE(readmitted.HasError());
            CHECK(readmitted.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
            REQUIRE(owner.world.Unload(World()).HasValue());
            CHECK(owner.world.RetiredCount() == 1);
            owner.FinishWorker();
            if (coordinator) {
                coordinator->CollectRetiredAtSafePoint();
                CHECK(coordinator->IsDrained());
            }
            CHECK(owner.world.CollectRetired() == NavigationWorldLifecycleState::Empty);
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
#if HORO_TEST_PHYSICS_NATIVE
        /** @brief Captures the published Detour route facts for the cover fixture. */
        QueryTestSample CaptureCoverRoute(const TacticalCoverPoint &point, const QueryStageId stage) {
            ProbeOwner navigation{Navigation::TestSupport::MakeGroundedProvider()};
            QueryNavigationProbe route{navigation.coordinator};
            const std::array callers{Caller()};
            auto input = Input(callers.front());
            input.request.start = {1, 0, 1};
            const auto destination = point.position.Millimeters();
            input.request.destination = {static_cast<float>(destination[0]) / 1000.0F, static_cast<float>(destination[1]) / 1000.0F,
                                         static_cast<float>(destination[2]) / 1000.0F};
            REQUIRE(route.Start(navigation.world, stage, input, 0).Value());
            navigation.Complete(callers, 1);
            const auto reachable = route.Poll(Current(callers));
            REQUIRE(reachable.HasValue());
            REQUIRE(reachable.Value());
            REQUIRE(reachable.Value()->filter == true);
            return *reachable.Value();
        }

        TEST_CASE("Cover combines actual reachability and occlusion without claiming occupancy", "[unit][ai][navigation][eqs][cover]") {
            auto scene = QueryTestSupport::MakeScene();
            const auto view = scene->View();
            const auto point = CoverPoint(*view.Find({1}));
            const auto occupant = *view.Find({2});
            TacticalCoverRegistry cover;
            REQUIRE(cover.Publish(view, 1, std::span{&point, 1}).HasValue());
            const auto candidates = cover.Capture(view, 1);
            REQUIRE(candidates.HasValue());
            REQUIRE(candidates.Value().size() == 1);
            const auto selection = candidates.Value().front().handle;

            const auto exposureStage = QueryStageId::Create(1).Value();
            const auto contentionStage = QueryStageId::Create(2).Value();
            const auto reachableStage = QueryStageId::Create(3).Value();
            const auto reachable = CaptureCoverRoute(point, reachableStage);

            SightTest::SightWorld physics;
            (void)physics.Box(-5);
            physics.Advance();
            const auto tick = physics.world->PublishedTick();
            const QueryPhysicsPublication publication{physics.capability.Identity(), 7, tick.publicationRevision, tick.completedTick};
            QueryPhysicsProbe sight;
            // The Physics fixture is rebased to a threat ten meters in front of the cover.
            const std::array rays{Physics::PhysicsRayQuery{.origin = {-0.25F, 0, 0}, .maximumDistanceMeters = 10},
                                  Physics::PhysicsRayQuery{.origin = {0.25F, 0, 0}, .maximumDistanceMeters = 10}};
            REQUIRE(sight.StartExposure(physics.capability, publication, exposureStage, rays, {.channel = SightTest::VisibilityChannel()})
                        .Value());
            REQUIRE(physics.world->ProcessQueryBatch().HasValue());
            const auto visibility = sight.Poll(publication);
            REQUIRE(visibility.HasValue());
            REQUIRE(visibility.Value().has_value());
            REQUIRE(visibility.Value()->filter == false);

            const auto scored = cover.Score(view, selection, reachable, *visibility.Value(), contentionStage);
            REQUIRE(scored.HasValue());
            CHECK(scored.Value()[0].stage == exposureStage);
            CHECK(scored.Value()[0].filter == true);
            CHECK(scored.Value()[1].stage == contentionStage);
            CHECK(scored.Value()[1].filter == true);
            CHECK(scored.Value()[2].stage == reachableStage);
            CHECK(scored.Value()[2].filter == true);
            const std::array observation{Examples::GuardCoverObservation{selection, reachable, *visibility.Value()}};
            const auto reserved = Examples::TrySelectGuardCover(cover, view, occupant, observation, contentionStage);
            REQUIRE(reserved.HasValue());
            REQUIRE(reserved.Value());
            CHECK(reserved.Value()->destination == point.position);
            const auto occupied = cover.Score(view, selection, reachable, *visibility.Value(), contentionStage);
            REQUIRE(occupied.HasValue());
            CHECK(occupied.Value()[1].filter == false);
        }
#endif

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
