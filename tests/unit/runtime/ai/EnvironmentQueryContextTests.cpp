#include "AiTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryContexts.h"
#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        [[nodiscard]] std::unique_ptr<Runtime::RuntimeScene> MakeScene(const std::uint64_t incarnation = 77) {
            Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
            for (const std::uint64_t id : {1ULL, 2ULL}) {
                Runtime::RuntimeEntityDefinition entity;
                entity.object = Runtime::SceneObjectId{id};
                builder.Add(entity);
            }
            const auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            auto created = Runtime::RuntimeScene::Create(definition.Value(), Runtime::SceneRuntimeId{incarnation});
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        [[nodiscard]] QueryContextCaptureSource Source(const Runtime::RuntimeSceneView &view, const std::uint64_t revision = 1) {
            const auto querier = view.Find(Runtime::SceneObjectId{1});
            const auto target = view.Find(Runtime::SceneObjectId{2});
            REQUIRE(querier.has_value());
            REQUIRE(target.has_value());
            return {.querier = *querier,
                    .querierLocation = Math::WorldCoordinate64::FromMillimeters(1'000, 2'000, 3'000),
                    .target = *target,
                    .targetLocation = Math::WorldCoordinate64::FromMillimeters(4'000, 5'000, 6'000),
                    .executionRevision = revision};
        }

        struct QueryFixture final {
            std::array<QueryItemTypeDescriptor, 1> items{{{MakeIdentity<QueryItemTypeId>(10),
                                                           {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(11), 1},
                                                           QueryItemKind::Point,
                                                           0}}};
            std::array<QueryContextDescriptor, 6> builtins{BuiltinQueryContextDescriptors()};
            QueryContextDescriptor custom{MakeIdentity<QueryContextId>(100),
                                          {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(101), 1},
                                          8};
            std::vector<QueryContextDescriptor> contexts{builtins.begin(), builtins.end()};
            QueryGeneratorDescriptor generator{MakeIdentity<QueryGeneratorId>(12),
                                               {QueryDescriptorSourceKind::Native, MakeIdentity<QueryProviderId>(11), 1},
                                               items[0].id,
                                               {1, 1},
                                               {},
                                               {}};

            QueryFixture() {
                contexts.push_back(custom);
            }

            [[nodiscard]] QuerySchemaRegistry Registry(std::span<const QueryTestDescriptor> tests = {}) const {
                auto result = QuerySchemaRegistry::Capture({items, contexts, std::span{&generator, 1}, tests});
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            [[nodiscard]] EnvironmentQueryPlan Plan(const QuerySchemaRegistry &registry, QueryTestId test = {},
                                                    bool reverseStages = false) const {
                QueryAssetSource source;
                source.id = MakeIdentity<QueryId>(13);
                source.result = {MakeIdentity<QueryResultSchemaId>(14), 1, items[0].id, {1, 1}, 8};
                source.stages = {{.id = MakeIdentity<QueryStageId>(15),
                                  .kind = QueryStageKind::Generator,
                                  .executionOrder = 1,
                                  .generator = generator.id,
                                  .descriptorVersion = {1, 1}}};
                if (test.IsValid())
                    source.stages.push_back({.id = MakeIdentity<QueryStageId>(16),
                                             .kind = QueryStageKind::Test,
                                             .executionOrder = reverseStages ? 3U : 2U,
                                             .test = test,
                                             .descriptorVersion = {1, 1}});
                if (reverseStages)
                    source.stages.push_back({.id = MakeIdentity<QueryStageId>(18),
                                             .kind = QueryStageKind::Test,
                                             .executionOrder = 2,
                                             .test = test,
                                             .descriptorVersion = {1, 1}});
                const auto asset = EnvironmentQueryAsset::Capture(source);
                REQUIRE(asset.HasValue());
                auto plan = EnvironmentQueryPlan::Compile(asset.Value(), registry);
                REQUIRE(plan.HasValue());
                return std::move(plan).Value();
            }
        };

        struct ExecutionProbe final {
            bool current{true};
            bool ready{true};
            bool backpressure{};
            int generated{};
            int attempts{};
            int polls{};
            int cancelled{};
            QueryStageId lastStage;
        };

        class ExecutionSource final : public IQueryExecutionSource {
        public:
            explicit ExecutionSource(ExecutionProbe &probe) : probe_(probe) {}

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return probe_.current;
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &, const QueryGeneratorOutput &output) override {
                ++probe_.generated;
                const std::array points{Math::WorldCoordinate64::FromMillimeters(1, 0, 0),
                                        Math::WorldCoordinate64::FromMillimeters(2, 0, 0)};
                return GenerateQueryPoints(points, output);
            }

            Result<bool> StartTest(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &item) override {
                ++probe_.attempts;
                probe_.lastStage = stage;
                if (probe_.backpressure)
                    return Result<bool>::Success(false);
                sample_ = {.stage = stage, .metric = static_cast<double>(std::get<Math::WorldCoordinate64>(item).Millimeters()[0]) / 2.0};
                return Result<bool>::Success(true);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                ++probe_.polls;
                return Result<std::optional<QueryTestSample>>::Success(probe_.ready ? std::exchange(sample_, std::nullopt) : std::nullopt);
            }

            void CancelTest() noexcept override {
                ++probe_.cancelled;
                sample_.reset();
            }

        private:
            ExecutionProbe &probe_;
            std::optional<QueryTestSample> sample_;
        };

        [[nodiscard]] std::unique_ptr<QueryExecution> MakeExecution(ExecutionProbe &probe, Runtime::RuntimeScene &scene,
                                                                    bool reverseStages = false) {
            QueryFixture fixture;
            const QueryTestDescriptor test{MakeIdentity<QueryTestId>(17), fixture.generator.origin, fixture.items[0].id, {1, 1}, {}, {}};
            const auto registry = fixture.Registry(std::span{&test, 1});
            const auto plan = fixture.Plan(registry, test.id, reverseStages);
            std::vector<QueryScoreRule> rules;
            for (const auto &stage : plan.Stages().subspan(1))
                rules.push_back({.stage = stage.id, .score = QueryScoreRange{}});
            auto scoring = QueryScoringPlan::Capture(plan, registry, rules, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto view = scene.View();
            const auto contexts = capture.Capture(plan, registry, providers.Value(), view, Source(view));
            REQUIRE(contexts.HasValue());
            auto execution =
                QueryExecution::Admit({{AiRuntimeIncarnation::Create(7).Value(), {1, 1}}, contexts.Value()},
                                      {plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1},
                                      std::move(scoring).Value(), {4, 8, 2, 2, 1, true}, std::make_unique<ExecutionSource>(probe));
            REQUIRE(execution.HasValue());
            return std::move(execution).Value();
        }

        TEST_CASE("EQS yields on shared budgets and reduces only completed work at the deadline", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget queryBudget{9}, agentBudget{8}, globalBudget{100};
            CHECK_FALSE(query->Sync(4));
            query->Advance(4, {queryBudget, agentBudget, globalBudget});
            CHECK(probe.generated == 0);
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {queryBudget, agentBudget, globalBudget});
            CHECK(probe.generated == 1);
            CHECK(probe.attempts == 0);
            CHECK(queryBudget.Remaining() == 1);
            CHECK(agentBudget.Remaining() == 0);
            CHECK(globalBudget.Remaining() == 92);
            AiWorkBudget shared{5};
            query->Advance(5, {shared, shared, shared});
            CHECK(probe.attempts == 0);
            CHECK_FALSE(query->Sync(6));
            probe.backpressure = true;
            query->Advance(6, {shared, shared, shared});
            CHECK(shared.Remaining() == 4);
            CHECK_FALSE(query->Sync(7));
            CHECK(probe.polls == 0);
            probe.backpressure = false;
            query->Advance(7, {shared, shared, shared});
            REQUIRE(query->Sync(8));
            CHECK(probe.attempts == 2);
            CHECK(probe.polls == 1);
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::PartialSuccess);
            CHECK(query->Published()->evaluated == 1);
            REQUIRE(query->Published()->winners.size() == 1);
            CHECK(query->Published()->winners[0].score.itemIndex == 0);
            CHECK_FALSE(query->Sync(9));
        }

        TEST_CASE("EQS retains pending work and retires it exactly once on cancellation or staleness", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe{.ready = false};
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget budget{9};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {budget, budget, budget});
            REQUIRE(probe.attempts == 1);
            CHECK_FALSE(query->Sync(6));
            AiWorkBudget empty{0};
            query->Advance(6, {empty, empty, empty});
            CHECK(probe.attempts == 1);
            CHECK(probe.polls == 1);
            QueryExecutionStatus expected{};
            SECTION("owner cancellation") {
                query->Cancel();
                query->Cancel();
                expected = QueryExecutionStatus::Cancelled;
            }
            SECTION("generation replacement") {
                probe.current = false;
                expected = QueryExecutionStatus::StaleInputs;
            }
            REQUIRE(query->Sync(7));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == expected);
            CHECK(query->Published()->winners.empty());
            CHECK(probe.cancelled == 1);
            CHECK(probe.polls == 1);
            CHECK_FALSE(query->Sync(8));
            query.reset();
            CHECK(probe.cancelled == 1);
        }

        TEST_CASE("EQS publishes completed tests in scored order", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget first{9}, second{2};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK_FALSE(query->Sync(6));
            query->Advance(6, {second, second, second});
            REQUIRE(query->Sync(7));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::Completed);
            CHECK(query->Published()->contextRevision == 1);
            CHECK(query->Published()->scene == scene->View().RuntimeId());
            REQUIRE(query->Published()->winners.size() == 2);
            CHECK(query->Published()->winners[0].score.itemIndex == 1);
            CHECK(query->Published()->winners[1].score.itemIndex == 0);
            CHECK(probe.cancelled == 0);
        }

        TEST_CASE("EQS dispatches authored stage order and scores by stable stage identity", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene, true);
            AiWorkBudget first{10}, second{1};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK(probe.lastStage == MakeIdentity<QueryStageId>(18));
            CHECK_FALSE(query->Sync(6));
            query->Advance(6, {second, second, second});
            CHECK(probe.lastStage == MakeIdentity<QueryStageId>(16));
            REQUIRE(query->Sync(7, true));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::PartialSuccess);
            REQUIRE(query->Published()->winners.size() == 1);
            CHECK(query->Published()->winners[0].score.scoreKey == QueryScoringPlan::ScoreKeyScale / 2);
        }

        TEST_CASE("EQS retires a pending provider when the publication window is missed", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene);
            AiWorkBudget first{9};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            REQUIRE(query->Sync(9));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::TimedOut);
            CHECK(probe.polls == 0);
            CHECK(probe.cancelled == 1);
        }

        struct ProviderState final {
            int calls{};
            QueryContextValue value{QueryCanonicalContext{}};
        };

        [[nodiscard]] Result<QueryContextValue> CaptureCustom(void *state, const Runtime::RuntimeSceneView &) {
            auto &provider = *static_cast<ProviderState *>(state);
            ++provider.calls;
            return Result<QueryContextValue>::Success(provider.value);
        }

        TEST_CASE("EQS lattice uses canonical ordering and rejects overflow without writing partial results",
                  "[unit][ai][eqs][generator]") {
            QueryFixture fixture;
            const auto center = BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation);
            fixture.generator.contexts = {{center, {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            const auto x = std::numeric_limits<std::int64_t>::max();
            source.querierLocation = Math::WorldCoordinate64::FromMillimeters(x, 17, 0);
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(contexts.HasValue());
            std::array<QueryGeneratedItem, 9> items;
            const auto untouched = items;
            QueryGeneratorOutput output{{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1}, 5, items};
            const QueryPointLattice grid{.stepMillimeters = 2, .outerSteps = 1};
            ExpectError(GenerateQueryLattice(*contexts.Value(), center, grid, output), AIErrors::EnvironmentQueryLimitExceeded);
            CHECK(items == untouched);
            output.maximumItems = 9;
            const auto generated = GenerateQueryLattice(*contexts.Value(), center, grid, output);
            REQUIRE(generated.HasValue());
            CHECK(generated.Value().provenance.stage == plan.Stages()[0].id);
            CHECK(generated.Value().invalidCoordinateCount == 3);
            REQUIRE(generated.Value().itemCount == 6);
            CHECK(items[0] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x - 2, 17, -2)});
            CHECK(items[5] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x, 17, 2)});
            const auto ring = GenerateQueryLattice(*contexts.Value(), center, {2, 1, 1}, output);
            REQUIRE(ring.HasValue());
            CHECK(ring.Value().invalidCoordinateCount == 1);
            CHECK(ring.Value().itemCount == 3);
            CHECK(items[0] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x, 17, -2)});
            CHECK(items[1] == QueryGeneratedItem{Math::WorldCoordinate64::FromMillimeters(x - 2, 17, 0)});
            ++output.provenance.revision;
            ExpectError(GenerateQueryLattice(*contexts.Value(), center, grid, output), AIErrors::EnvironmentQueryContextStale);
        }

        TEST_CASE("EQS copies bounded provider points in declared order", "[unit][ai][eqs][generator]") {
            const std::array points{Math::WorldCoordinate64::FromMillimeters(123, 0, 0),
                                    Math::WorldCoordinate64::FromMillimeters(-42, 0, 0)};
            std::array<QueryGeneratedItem, 2> items;
            QueryGeneratorOutput output{{MakeIdentity<QueryStageId>(1), MakeIdentity<QueryGeneratorId>(2), MakeIdentity<QueryProviderId>(3),
                                         19},
                                        1,
                                        items};
            ExpectError(GenerateQueryPoints(points, output), AIErrors::EnvironmentQueryLimitExceeded);
            output.maximumItems = 2;
            const auto generated = GenerateQueryPoints(points, output);
            REQUIRE(generated.HasValue());
            CHECK(generated.Value().itemCount == 2);
            CHECK(generated.Value().provenance.revision == 19);
            CHECK(items[0] == QueryGeneratedItem{points[0]});
            CHECK(items[1] == QueryGeneratedItem{points[1]});
        }

        TEST_CASE("EQS contexts capture immutable built-ins once per execution revision", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            for (std::uint8_t index = 0; index < static_cast<std::uint8_t>(BuiltinQueryContext::Count); ++index)
                fixture.generator.contexts.push_back({BuiltinQueryContextId(static_cast<BuiltinQueryContext>(index)), {1, 1}});
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            REQUIRE(plan.RequiredContexts().size() == 6);
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            const std::array group{source.querier, *source.target};
            source.group = group;
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto first = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(first.HasValue());
            REQUIRE(first.Value()->Values().size() == 6);
            CHECK(first.Value()->Scene() == view.RuntimeId());
            for (std::size_t index = 0; index < plan.RequiredContexts().size(); ++index)
                CHECK(first.Value()->Values()[index].id == plan.RequiredContexts()[index].id);
            CHECK(std::get<Runtime::EntityRef>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Querier))->value) ==
                  source.querier);
            CHECK(std::get<Math::WorldCoordinate64>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin))->value) ==
                  Math::WorldCoordinate64{});
            CHECK(
                std::get<std::vector<Runtime::EntityRef>>(first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Group))->value) ==
                std::vector<Runtime::EntityRef>{group.begin(), group.end()});

            source.querierLocation = Math::WorldCoordinate64::FromMillimeters(99, 99, 99);
            const auto repeated = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(repeated.HasValue());
            CHECK(repeated.Value() == first.Value());
            source.executionRevision = 2;
            const auto advanced = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(advanced.HasValue());
            CHECK(advanced.Value() != first.Value());
            CHECK(std::get<Math::WorldCoordinate64>(
                      first.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::QuerierLocation))->value) ==
                  Math::WorldCoordinate64::FromMillimeters(1'000, 2'000, 3'000));
            source.executionRevision = 1;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);

            Runtime::SceneCommandBuffer destroy;
            destroy.Destroy(*source.target);
            REQUIRE(scene->Commit(std::move(destroy)).HasValue());
            source.executionRevision = 3;
            ExpectError(capture.Capture(plan, registry, providers.Value(), scene->View(), source), AIErrors::EnvironmentQueryContextStale);
            CHECK(first.Value()->Values().size() == 6);
        }

        TEST_CASE("EQS present empty group differs from a missing required group", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::Group), {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            source.group = std::span<const Runtime::EntityRef>{};
            QueryContextCapture capture;
            const auto emptyGroup = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(emptyGroup.HasValue());
            CHECK(std::get<std::vector<Runtime::EntityRef>>(
                      emptyGroup.Value()->Find(BuiltinQueryContextId(BuiltinQueryContext::Group))->value)
                      .empty());
            source.group.reset();
            source.executionRevision = 2;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextMissing);
            source.group = std::span<const Runtime::EntityRef>{};
            const auto retried = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(retried.HasValue());
            CHECK(retried.Value()->Revision() == 2);
            CHECK(emptyGroup.Value()->Revision() == 1);
        }

        TEST_CASE("EQS reserved built-in context IDs reject foreign descriptors", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::Querier), {1, 1}}};
            fixture.contexts[0].origin = {QueryDescriptorSourceKind::Package, MakeIdentity<QueryProviderId>(99), 1};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, Source(view)), AIErrors::EnvironmentQueryContextInvalid);
        }

        TEST_CASE("EQS required target and scene generations fail closed", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{BuiltinQueryContextId(BuiltinQueryContext::TargetLocation), {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            source.target.reset();
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextMissing);
            source = Source(view);
            source.target->entity.generation += 1;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);
            source = Source(view);
            source.querier.runtime = Runtime::SceneRuntimeId{88};
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextStale);
        }

        TEST_CASE("EQS custom provider registration validates capability and canonical bounds", "[unit][ai][eqs][context]") {
            QueryFixture fixture;
            fixture.generator.contexts = {{fixture.custom.id, {1, 1}}};
            const auto registry = fixture.Registry();
            const auto plan = fixture.Plan(registry);
            ProviderState state;
            auto canonical = QueryCanonicalContext{};
            canonical.size = 4;
            canonical.bytes[0] = std::byte{1};
            state.value = canonical;
            const QueryContextProviderRegistration registration{.id = fixture.custom.id,
                                                                .schemaVersion = 1,
                                                                .requiredCapabilities = AiCapabilitySet::Of(AiCapability::Perception),
                                                                .state = &state,
                                                                .capture = CaptureCustom};
            const auto providers = QueryContextProviderRegistry::Capture(std::array{registration}, registry);
            REQUIRE(providers.HasValue());
            ExpectError(QueryContextProviderRegistry::Capture(std::array{registration, registration}, registry),
                        AIErrors::EnvironmentQueryIdentityConflict);
            auto scene = MakeScene();
            const auto view = scene->View();
            auto source = Source(view);
            QueryContextCapture capture;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source),
                        AIErrors::EnvironmentQueryContextCapabilityUnavailable);
            CHECK(state.calls == 0);
            source.availableCapabilities = AiCapabilitySet::Of(AiCapability::Perception);
            const auto captured = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(captured.HasValue());
            CHECK(state.calls == 1);
            CHECK(std::get<QueryCanonicalContext>(captured.Value()->Find(fixture.custom.id)->value).size == 4);
            CHECK(capture.Capture(plan, registry, providers.Value(), view, source).Value() == captured.Value());
            CHECK(state.calls == 1);

            canonical.size = 9;
            state.value = canonical;
            source.executionRevision = 2;
            ExpectError(capture.Capture(plan, registry, providers.Value(), view, source), AIErrors::EnvironmentQueryContextInvalid);
            CHECK(captured.Value()->Revision() == 1);
            CHECK(state.calls == 2);
            canonical.size = 4;
            state.value = canonical;
            const auto retried = capture.Capture(plan, registry, providers.Value(), view, source);
            REQUIRE(retried.HasValue());
            CHECK(retried.Value()->Revision() == 2);
            CHECK(state.calls == 3);
        }
    }  // namespace
}  // namespace Horo::AI
