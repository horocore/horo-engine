#include "AllocationProbe.h"
#include "EnvironmentQueryTestSupport.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/DecisionQueryTask.h"
#include "Horo/AI/EnvironmentQueryCache.h"
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
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::AI {
    namespace {
        using namespace QueryTestSupport;

        enum class SourceFailure {
            None,
            Generation,
            Admission,
            Poll,
            UnknownPoll,
            DiagnosticPoll,
            TypedAdmission
        };

        struct ExecutionProbe final {
            bool current{true};
            bool ready{true};
            bool backpressure{};
            bool overrun{};
            bool generationOverrun{};
            SourceFailure failure{SourceFailure::None};
            int generated{};
            int attempts{};
            int polls{};
            int cancelled{};
            CancellationSource *revokeOnStart{};
            CancellationSource *revokeOnPoll{};
            QueryStageId lastStage;
            std::optional<Runtime::EntityRef> actor;
            std::size_t pointCount{2};
            std::optional<bool> filter;
        };

        class ExecutionSource final : public IQueryExecutionSource {
        public:
            explicit ExecutionSource(ExecutionProbe &probe) : probe_(probe) {}

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return probe_.current;
            }

            Result<QueryGenerationResult> Generate(const QueryExecutionInputs &, const QueryGeneratorOutput &output) override {
                ++probe_.generated;
                if (probe_.failure == SourceFailure::Generation)
                    throw std::runtime_error("generator failed");
                if (probe_.actor) {
                    output.items[0] = *probe_.actor;
                    return Result<QueryGenerationResult>::Success({.provenance = output.provenance, .itemCount = 1});
                }
                REQUIRE(probe_.pointCount <= output.items.size());
                for (std::size_t index = 0; index < probe_.pointCount; ++index)
                    output.items[index] = Math::WorldCoordinate64::FromMillimeters(static_cast<std::int64_t>(index + 1), 0, 0);
                return Result<QueryGenerationResult>::Success({.provenance = output.provenance, .itemCount = probe_.pointCount});
            }

            Result<bool> StartTest(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &item) override {
                ++probe_.attempts;
                if (probe_.failure == SourceFailure::Admission)
                    throw std::runtime_error("test admission failed");
                if (probe_.failure == SourceFailure::TypedAdmission)
                    return Result<bool>::Failure(MakeError(AIErrors::CapabilityUnavailable));
                probe_.lastStage = stage;
                if (probe_.backpressure)
                    return Result<bool>::Success(false);
                const auto *point = std::get_if<Math::WorldCoordinate64>(&item);
                sample_ = {.stage = stage,
                           .filter = probe_.filter,
                           .metric = point ? static_cast<double>(point->Millimeters()[0]) / 2.0 : 1.0};
                if (probe_.revokeOnStart)
                    probe_.revokeOnStart->RequestCancellation();
                return Result<bool>::Success(true);
            }

            Result<std::optional<QueryTestSample>> PollTest() override {
                ++probe_.polls;
                if (probe_.revokeOnPoll)
                    probe_.revokeOnPoll->RequestCancellation();
                if (probe_.failure == SourceFailure::Poll)
                    throw std::runtime_error("poll failed");
                if (probe_.failure == SourceFailure::DiagnosticPoll) {
                    struct DiagnosticFailure final : std::exception {
                        const char *what() const noexcept override {
                            return "Provider polling failed with a native diagnostic that requires owned string allocation at the query "
                                   "boundary.";
                        }
                    };

                    throw DiagnosticFailure{};
                }
                if (probe_.failure == SourceFailure::UnknownPoll)
                    throw 42;
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

        class OwnerSource final : public IOwnerQueryExecutionSource {
        public:
            explicit OwnerSource(ExecutionProbe &probe) : IOwnerQueryExecutionSource({2, 1}), probe_(probe) {}

            bool IsCurrent(const QueryExecutionInputs &) const noexcept override {
                return probe_.current;
            }

        protected:
            Result<QueryGenerationResult> GenerateAtOwner(const QueryExecutionInputs &, const QueryGeneratorOutput &output,
                                                          AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(probe_.generationOverrun ? 3 : 2);
                ++probe_.generated;
                const std::array points{Math::WorldCoordinate64::FromMillimeters(1, 0, 0),
                                        Math::WorldCoordinate64::FromMillimeters(2, 0, 0)};
                return GenerateQueryPoints(points, output);
            }

            Result<QueryTestSample> TestAtOwner(const QueryExecutionInputs &, const QueryStageId stage, const QueryGeneratedItem &item,
                                                AiWorkBudget &budget) noexcept override {
                (void)budget.Consume(probe_.overrun ? 2 : 1);
                ++probe_.attempts;
                return Result<QueryTestSample>::Success(
                    {.stage = stage, .metric = static_cast<double>(std::get<Math::WorldCoordinate64>(item).Millimeters()[0]) / 2.0});
            }

        private:
            ExecutionProbe &probe_;
        };

        class ScheduledQuery final : public IAiScheduledDecision {
        public:
            explicit ScheduledQuery(QueryExecution &query) : query_(query) {}

            Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                AiWorkBudget queryBudget{10};
                query_.Advance(context.tick, {queryBudget, budget, budget});
                return Result<AiSliceOutcome>::Success(AiSliceOutcome::Complete);
            }

            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                query_.Cancel();
            }

        private:
            QueryExecution &query_;
        };

        [[nodiscard]] std::unique_ptr<QueryExecution> MakeExecution(ExecutionProbe &probe, Runtime::RuntimeScene &scene,
                                                                    bool reverseStages = false, bool deterministic = false,
                                                                    QueryExecutionInputs *capturedInputs = nullptr,
                                                                    QueryDebugCaptureSettings debugCapture = {},
                                                                    CancellationToken ownerCancellation = {}) {
            QueryFixture fixture;
            if (probe.actor)
                fixture.items.front().kind = QueryItemKind::Actor;
            const QueryTestDescriptor test{MakeIdentity<QueryTestId>(17), fixture.generator.origin, fixture.items[0].id, {1, 1}, {}, {}};
            const auto registry = fixture.Registry(std::span{&test, 1});
            const auto plan = fixture.Plan(registry, test.id, reverseStages);
            std::vector<QueryScoreRule> rules;
            for (const auto &stage : plan.Stages().subspan(1))
                rules.push_back({.stage = stage.id,
                                 .requiredFilterValue = probe.filter.has_value() ? std::optional{true} : std::nullopt,
                                 .score = QueryScoreRange{}});
            auto scoring = QueryScoringPlan::Capture(plan, registry, rules, QueryScoreTieBreak::StableIndex);
            REQUIRE(scoring.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry);
            REQUIRE(providers.HasValue());
            QueryContextCapture capture;
            const auto view = scene.View();
            const auto contexts = capture.Capture(plan, registry, providers.Value(), view, Source(view));
            REQUIRE(contexts.HasValue());
            const QueryExecutionInputs inputs{{AiRuntimeIncarnation::Create(7).Value(), {1, 1}},
                                              contexts.Value(),
                                              std::move(ownerCancellation)};
            if (capturedInputs)
                *capturedInputs = inputs;
            const QueryGeneratorProvenance provenance{plan.Stages()[0].id, fixture.generator.id, fixture.generator.origin.provider, 1};
            const auto admit = [&]() {
                if (deterministic)
                    return QueryExecution::AdmitDeterministic(inputs, provenance, std::move(scoring).Value(),
                                                              {4, 8, 2, 2, 2, true, debugCapture}, std::make_unique<OwnerSource>(probe));
                return QueryExecution::Admit(inputs, provenance, std::move(scoring).Value(),
                                             {4, 8, probe.pointCount, 2, 1, true, debugCapture}, std::make_unique<ExecutionSource>(probe));
            };
            auto execution = admit();
            REQUIRE(execution.HasValue());
            return std::move(execution).Value();
        }

        class QueryTaskHost final : public IDecisionQueryTaskHost {
        public:
            struct NativeQuery final {
                EnvironmentQueryPlan plan;
                QuerySchemaRegistry registry;
            };

            Runtime::RuntimeScene &scene;
            ExecutionProbe probe;
            std::size_t admissions{};
            CancellationSource *revokeAdmission{};
            std::optional<NativeQuery> native;

            explicit QueryTaskHost(Runtime::RuntimeScene &value) : scene(value) {}

            Result<std::unique_ptr<QueryExecution>> Admit(const DecisionQueryTaskDescriptor &, const AiTaskOperationContext &operation,
                                                          std::uint64_t submittedTick, std::uint64_t deadlineTick) override {
                ++admissions;
                if (native.has_value()) {
                    const auto view = scene.View();
                    auto source = Source(view);
                    source.querierLocation = Math::WorldCoordinate64::FromMillimeters(1200, 0, 0);
                    const auto providers = QueryContextProviderRegistry::Capture({}, native->registry);
                    REQUIRE(providers.HasValue());
                    QueryContextCapture capture;
                    const auto contexts = capture.Capture(native->plan, native->registry, providers.Value(), view, source);
                    REQUIRE(contexts.HasValue());
                    return AdmitBuiltinPointQuery(native->plan, native->registry, view,
                                                  {operation.agent, contexts.Value(), operation.ownerCancellation},
                                                  {.submittedTick = submittedTick,
                                                   .deadlineTick = deadlineTick,
                                                   .maximumCandidates = 9,
                                                   .bestCount = 1});
                }
                if (revokeAdmission)
                    revokeAdmission->RequestCancellation();
                return Result<std::unique_ptr<QueryExecution>>::Success(MakeExecution(probe, scene));
            }
        };

        struct QueryTaskFixture final {
            std::unique_ptr<Runtime::RuntimeScene> scene{MakeScene()};
            QueryTaskHost host{*scene};
            AgentHandle agent{MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}};
            DecisionNodeId node{MakeIdentity<DecisionNodeId>(5)};
            TaskId task{MakeIdentity<TaskId>(6)};
            BlackboardKeyId output{MakeIdentity<BlackboardKeyId>(7)};
            std::unique_ptr<BlackboardInstance> blackboard;
            DecisionQueryTask *executor{};

            std::shared_ptr<const BehaviorTreeExecutionPlan> BuildTree(const BlackboardValueKind kind) {
                const std::array keys{BlackboardKeyDescriptor{.key = output, .kind = kind, .presence = BlackboardKeyPresence::Optional}};
                auto schema = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(8), .keys = keys});
                REQUIRE(schema.HasValue());
                auto owned = std::make_shared<const BlackboardSchema>(std::move(schema).Value());
                auto created = BlackboardInstance::Create({agent, owned->Identity(), owned->Version(), 1, 1}, owned);
                REQUIRE(created.HasValue());
                blackboard = std::move(created).Value();
                const SourceLocation authored{"tests/query_task", 1, 1};
                const auto type = MakeIdentity<DecisionNodeTypeId>(10);
                const DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(11),
                                                    .kind = DecisionPlanKind::BehaviorTree,
                                                    .blackboardSchema = owned->Identity(),
                                                    .nodes = {{.id = node, .type = type, .source = authored}},
                                                    .source = authored};
                const std::array descriptors{
                    DecisionNodeDescriptor{.type = type,
                                           .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(12), 1},
                                           .requirements = {{.key = output,
                                                             .kind = kind,
                                                             .access = BlackboardKeyAccess::ReadWrite,
                                                             .source = authored}},
                                           .source = authored}};
                const auto bindings = DecisionAssetCompiler::Compile(asset, {}, descriptors, std::array{owned});
                REQUIRE(bindings.HasValue());
                REQUIRE(bindings.Value().IsValid());
                const std::array nodes{BehaviorTreeExecutionNode{.id = node, .operation = BehaviorTreeOperation::Task, .task = task}};
                const auto tree = BehaviorTreeExecutionPlan::Compile(bindings.Value().plan, node, nodes, {});
                REQUIRE(tree.HasValue());
                return tree.Value();
            }

            Result<std::unique_ptr<DecisionQueryTask>> Bind(const BehaviorTreeExecutionPlan &tree) {
                if (host.native.has_value()) {
                    const auto &plan = host.native->plan;
                    return DecisionQueryTask::Bind({node, task, plan.Id(), plan.ResultSchema().id, plan.ResultSchema().version,
                                                    plan.ResultSchema().itemType, output, 30},
                                                   plan, host.native->registry, tree, host);
                }
                QueryFixture fixture;
                if (host.probe.actor)
                    fixture.items.front().kind = QueryItemKind::Actor;
                const auto registry = fixture.Registry();
                const auto plan = fixture.Plan(registry);
                return DecisionQueryTask::Bind({node, task, plan.Id(), plan.ResultSchema().id, plan.ResultSchema().version,
                                                plan.ResultSchema().itemType, output, 4},
                                               plan, registry, tree, host);
            }

            std::unique_ptr<BehaviorTreeInstance> Start(const BlackboardValueKind kind = BlackboardValueKind::WorldCoordinate,
                                                        CancellationToken owner = {}) {
                const auto tree = BuildTree(kind);
                auto bound = Bind(*tree);
                REQUIRE(bound.HasValue());
                auto implementation = std::move(bound).Value();
                executor = implementation.get();
                auto instance = BehaviorTreeInstance::Create(tree,
                                                             {.agent = agent,
                                                              .firstTaskSlot = 100,
                                                              .blackboard = blackboard->Binding(),
                                                              .ownerCancellation = std::move(owner)},
                                                             std::move(implementation));
                REQUIRE(instance.HasValue());
                return std::move(instance).Value();
            }

            AiTaskState Evaluate(BehaviorTreeInstance &instance, const std::uint64_t tick) {
                const auto snapshot = blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                const auto evaluated = instance.Evaluate(tick, snapshot.Value(), agent);
                REQUIRE(evaluated.HasValue());
                return evaluated.Value();
            }
        };

        TEST_CASE("EQS task rejects incompatible compiled output before activating the behavior tree", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            const auto tree = fixture.BuildTree(BlackboardValueKind::Boolean);
            ExpectError(fixture.Bind(*tree), AIErrors::BlackboardValueTypeMismatch);
            CHECK(fixture.host.admissions == 0);
        }

        TEST_CASE("EQS admission revoked by its owner releases the query without publishing a running task", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            const auto tree = fixture.BuildTree(BlackboardValueKind::WorldCoordinate);
            const auto executor = fixture.Bind(*tree);
            REQUIRE(executor.HasValue());
            const auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const BehaviorTreeEvaluationContext context{fixture.node, tree->Bindings().BindingsForNode(fixture.node), snapshot.Value(), 4};
            CancellationSource owner;
            fixture.host.revokeAdmission = &owner;
            const AiTaskOperationContext operation{.taskDefinition = fixture.task,
                                                   .task = {fixture.agent.incarnation, {2, 1}},
                                                   .agent = fixture.agent,
                                                   .ownerCancellation = owner.Token()};
            const auto cancelled = executor.Value()->Start(context, operation);
            REQUIRE(cancelled.HasValue());
            CHECK(cancelled.Value() == AiTaskState::Cancelled);
            CHECK(fixture.host.admissions == 1);
            CHECK(fixture.host.probe.generated == 0);

            fixture.host.revokeAdmission = nullptr;
            auto replacement = operation;
            replacement.task.slot.generation++;
            replacement.ownerCancellation = {};
            const auto started = executor.Value()->Start(context, replacement);
            REQUIRE(started.HasValue());
            CHECK(started.Value() == AiTaskState::Running);
            CHECK(fixture.host.admissions == 2);
        }

        TEST_CASE("Behavior tree EQS task awaits owner phases and commits the winner once", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            auto tree = fixture.Start();
            REQUIRE(fixture.Evaluate(*tree, 4) == AiTaskState::Running);
            CHECK(fixture.host.probe.generated == 0);
            for (std::uint64_t tick = 5; tick <= 6; ++tick) {
                fixture.executor->SyncQuery(tick);
                REQUIRE(fixture.Evaluate(*tree, tick) == AiTaskState::Running);
                AiWorkBudget budget{64};
                fixture.executor->AdvanceQuery(tick, {budget, budget, budget});
            }
            fixture.executor->SyncQuery(7);
            REQUIRE(fixture.Evaluate(*tree, 7) == AiTaskState::Running);
            const auto view = fixture.scene->View();
            CHECK_FALSE(fixture.executor->CommitOutput(7, view, *fixture.blackboard).Value());
            REQUIRE(fixture.executor->CommitOutput(8, view, *fixture.blackboard).Value());
            REQUIRE(fixture.Evaluate(*tree, 8) == AiTaskState::Succeeded);
            CHECK_FALSE(fixture.executor->CommitOutput(9, view, *fixture.blackboard).Value());
            const auto value = fixture.blackboard->Snapshot().Value().Read(fixture.output);
            REQUIRE(value.HasValue());
            REQUIRE(value.Value().has_value());
            CHECK(std::get<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*value.Value())).Millimeters()[0] == 2);
        }

        TEST_CASE("EQS native admission refuses a registry that declares actor output for a point lattice", "[unit][ai][eqs][task]") {
            const auto builtin = CaptureBuiltinQuerySchema();
            REQUIRE(builtin.HasValue());
            const auto lattice = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(20), MakeIdentity<QueryStageId>(21),
                                                           MakeIdentity<QueryResultSchemaId>(22), "Native points", {1000, 1}, 9);
            REQUIRE(lattice.HasValue());
            const auto *item = builtin.Value().Find(lattice.Value().ResultSchema().itemType);
            const auto *generator = builtin.Value().Find(lattice.Value().Stages().front().generator);
            REQUIRE(item);
            REQUIRE(generator);
            auto actor = *item;
            actor.kind = QueryItemKind::Actor;
            const auto registrations = BuiltinQueryContextDescriptors();
            const auto registry = QuerySchemaRegistry::Capture({std::span{&actor, 1}, registrations, std::span{generator, 1}, {}});
            REQUIRE(registry.HasValue());
            const auto plan = EnvironmentQueryPlan::Compile(lattice.Value(), registry.Value());
            REQUIRE(plan.HasValue());
            const auto providers = QueryContextProviderRegistry::Capture({}, registry.Value());
            REQUIRE(providers.HasValue());
            const auto scene = MakeScene();
            const auto view = scene->View();
            QueryContextCapture capture;
            const auto contexts = capture.Capture(plan.Value(), registry.Value(), providers.Value(), view, Source(view));
            REQUIRE(contexts.HasValue());
            ExpectError(AdmitBuiltinPointQuery(plan.Value(), registry.Value(), view,
                                               {{MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}}, contexts.Value()},
                                               {.submittedTick = 4, .deadlineTick = 8, .maximumCandidates = 9, .bestCount = 1}),
                        AIErrors::EnvironmentQueryDescriptorUnavailable);
        }

        TEST_CASE("Behavior tree EQS commits the real native lattice winner to its typed output", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            auto registry = CaptureBuiltinQuerySchema();
            REQUIRE(registry.HasValue());
            const auto lattice = CreateBuiltinLatticeQuery(MakeIdentity<QueryId>(20), MakeIdentity<QueryStageId>(21),
                                                           MakeIdentity<QueryResultSchemaId>(22), "Native points", {1000, 1}, 9);
            REQUIRE(lattice.HasValue());
            const auto origin = BuiltinQueryContextId(BuiltinQueryContext::WorldOrigin);
            const auto authored = AddBuiltinPointDistanceTest(lattice.Value(), MakeIdentity<QueryStageId>(23), {1000, true, 1, origin});
            REQUIRE(authored.HasValue());
            auto compiled = EnvironmentQueryPlan::Compile(authored.Value(), registry.Value());
            REQUIRE(compiled.HasValue());
            fixture.host.native.emplace(std::move(compiled).Value(), std::move(registry).Value());

            const auto tree = fixture.Start();
            REQUIRE(fixture.Evaluate(*tree, 4) == AiTaskState::Running);
            const auto view = fixture.scene->View();
            SECTION("a retired Scene revision cannot write output") {
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands.Create({}).IsValid());
                REQUIRE(fixture.scene->Commit(std::move(commands)).HasValue());
                fixture.executor->SyncQuery(5);
                REQUIRE(fixture.Evaluate(*tree, 5) == AiTaskState::Cancelled);
                const auto value = fixture.blackboard->Snapshot().Value().Read(fixture.output);
                REQUIRE(value.HasValue());
                CHECK_FALSE(value.Value().has_value());
            }
            SECTION("the first complete winner reaches the declared destination") {
                bool committed = false;
                for (std::uint64_t tick = 5; tick < 34; ++tick) {
                    fixture.executor->SyncQuery(tick);
                    const auto output = fixture.executor->CommitOutput(tick, view, *fixture.blackboard);
                    REQUIRE(output.HasValue());
                    if (output.Value()) {
                        CHECK(fixture.Evaluate(*tree, tick) == AiTaskState::Succeeded);
                        committed = true;
                        break;
                    }
                    REQUIRE(fixture.Evaluate(*tree, tick) == AiTaskState::Running);
                    AiWorkBudget budget{128};
                    fixture.executor->AdvanceQuery(tick, {budget, budget, budget});
                }
                REQUIRE(committed);
                const auto value = fixture.blackboard->Snapshot().Value().Read(fixture.output);
                REQUIRE(value.HasValue());
                REQUIRE(value.Value().has_value());
                CHECK(std::get<Math::WorldCoordinate64>(std::get<BlackboardScalarValue>(*value.Value())) ==
                      Math::WorldCoordinate64::FromMillimeters(200, 0, 0));
            }
        }

        TEST_CASE("Behavior tree EQS revalidates the exact winning actor at BlackboardSync", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            const auto actor = *fixture.scene->View().Find({2});
            fixture.host.probe.actor = actor;
            auto tree = fixture.Start(BlackboardValueKind::EntityReference);
            REQUIRE(fixture.Evaluate(*tree, 4) == AiTaskState::Running);
            fixture.executor->SyncQuery(5);
            AiWorkBudget budget{64};
            fixture.executor->AdvanceQuery(5, {budget, budget, budget});
            fixture.executor->SyncQuery(6);
            REQUIRE(fixture.Evaluate(*tree, 6) == AiTaskState::Running);
            SECTION("current actor is written") {
                REQUIRE(fixture.executor->CommitOutput(7, fixture.scene->View(), *fixture.blackboard).Value());
                REQUIRE(fixture.Evaluate(*tree, 7) == AiTaskState::Succeeded);
                const auto value = fixture.blackboard->Snapshot().Value().Read(fixture.output);
                REQUIRE(value.HasValue());
                REQUIRE(value.Value().has_value());
                CHECK(std::get<BlackboardStoredEntityReference>(std::get<BlackboardScalarValue>(*value.Value())) ==
                      BlackboardStoredEntityReference{actor.runtime.value, actor.entity.index, actor.entity.generation});
            }
            SECTION("retired actor is rejected") {
                Runtime::SceneCommandBuffer destroy;
                destroy.Destroy(actor);
                REQUIRE(fixture.scene->Commit(destroy).HasValue());
                ExpectError(fixture.executor->CommitOutput(7, fixture.scene->View(), *fixture.blackboard),
                            AIErrors::EnvironmentQueryContextStale);
                CHECK_FALSE(fixture.blackboard->Snapshot().Value().Read(fixture.output).Value());
            }
            SECTION("abort wins after result publication and before BlackboardSync") {
                REQUIRE(tree->AbortSubtree(fixture.node).HasValue());
                CHECK_FALSE(fixture.executor->CommitOutput(7, fixture.scene->View(), *fixture.blackboard).Value());
                REQUIRE(fixture.Evaluate(*tree, 7) == AiTaskState::Cancelled);
            }
        }

        TEST_CASE("Behavior tree EQS abort and deadline retire work before any late output", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            fixture.host.probe.ready = false;
            auto tree = fixture.Start();
            REQUIRE(fixture.Evaluate(*tree, 4) == AiTaskState::Running);
            fixture.executor->SyncQuery(5);
            AiWorkBudget budget{64};
            fixture.executor->AdvanceQuery(5, {budget, budget, budget});
            REQUIRE(fixture.host.probe.attempts == 1);
            SECTION("explicit subtree abort") {
                REQUIRE(tree->AbortSubtree(fixture.node).HasValue());
                REQUIRE(fixture.Evaluate(*tree, 6) == AiTaskState::Cancelled);
            }
            SECTION("query reaches its admitted deadline") {
                for (std::uint64_t tick = 6; tick < 8; ++tick) {
                    fixture.executor->SyncQuery(tick);
                    REQUIRE(fixture.Evaluate(*tree, tick) == AiTaskState::Running);
                }
                fixture.executor->SyncQuery(8);
                REQUIRE(fixture.Evaluate(*tree, 8) == AiTaskState::Cancelled);
            }
            fixture.host.probe.ready = true;
            fixture.executor->SyncQuery(9);
            CHECK(fixture.host.probe.cancelled == 1);
            CHECK_FALSE(fixture.executor->CommitOutput(9, fixture.scene->View(), *fixture.blackboard).Value());
            CHECK_FALSE(fixture.blackboard->Snapshot().Value().Read(fixture.output).Value());
        }

        TEST_CASE("EQS retires provider work within the owner phase that revokes its agent", "[unit][ai][eqs][task]") {
            QueryTaskFixture fixture;
            CancellationSource owner;
            fixture.host.probe.ready = false;
            auto tree = fixture.Start(BlackboardValueKind::WorldCoordinate, owner.Token());
            REQUIRE(fixture.Evaluate(*tree, 4) == AiTaskState::Running);
            fixture.executor->SyncQuery(5);
            AiWorkBudget budget{64};
            SECTION("revocation during provider start") {
                fixture.host.probe.revokeOnStart = &owner;
                fixture.executor->AdvanceQuery(5, {budget, budget, budget});
            }
            SECTION("revocation during provider polling") {
                fixture.executor->AdvanceQuery(5, {budget, budget, budget});
                REQUIRE(fixture.host.probe.cancelled == 0);
                fixture.host.probe.revokeOnPoll = &owner;
                fixture.executor->SyncQuery(6);
            }
            REQUIRE(owner.Token().IsCancellationRequested());
            CHECK(fixture.host.probe.attempts == 1);
            CHECK(fixture.host.probe.cancelled == 1);
            REQUIRE(fixture.Evaluate(*tree, 6) == AiTaskState::Cancelled);
            CHECK_FALSE(fixture.executor->CommitOutput(7, fixture.scene->View(), *fixture.blackboard).Value());
            CHECK(fixture.host.probe.cancelled == 1);
        }

        TEST_CASE("EQS cache retains complete results under exact scene, dependency and owner bounds", "[unit][ai][eqs][cache]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            ExecutionSource authority{probe};
            QueryExecutionInputs inputs;
            auto query = MakeExecution(probe, *scene, false, false, &inputs);
            for (std::uint64_t tick = 5; tick <= 8 && !query->Published(); ++tick) {
                if (!query->Sync(tick)) {
                    AiWorkBudget budget{64};
                    query->Advance(tick, {budget, budget, budget});
                }
            }
            const auto *result = query->Published();
            REQUIRE(result);
            REQUIRE(result->status == QueryExecutionStatus::Completed);
            const auto tick = result->publicationTick;
            auto created = QueryResultCache::Create(scene->View().RuntimeId(), {1, 8192, 3});
            REQUIRE(created.HasValue());
            auto cache = std::move(created).Value();
            std::array canonical{std::byte{1}};
            std::array dependencies{QueryCacheDependency{MakeIdentity<QueryProviderId>(1), 1, 2, 0, 3}};
            QueryCacheKey key{inputs.contexts->Query(), {}, canonical, dependencies, true};
            key.artifact.bytes[0] = 1;
            REQUIRE(cache.Store(scene->View(), inputs, authority, key, *result, tick).Value());
            const auto retained = cache.Lookup(scene->View(), inputs, authority, key, tick);
            REQUIRE(retained);
            CHECK(retained->agent == inputs.agent);
            CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick + 3));

            SECTION("live dependency, authority and Scene must match") {
                ++dependencies[0].revision;
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                --dependencies[0].revision;
                ++dependencies[0].incarnation;
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                --dependencies[0].incarnation;
                key.completeFootprint = false;
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                key.completeFootprint = true;
                auto otherScene = MakeScene(88);
                CHECK_FALSE(cache.Lookup(otherScene->View(), inputs, authority, key, tick));
                probe.current = false;
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
            }
            SECTION("eviction never invalidates caller snapshots and private data cannot cross agents") {
                auto other = inputs;
                ++other.agent.slot.generation;
                CHECK_FALSE(cache.Lookup(scene->View(), other, authority, key, tick));
                canonical[0] = std::byte{2};
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                REQUIRE(cache.Store(scene->View(), inputs, authority, key, *result, tick).Value());
                canonical[0] = std::byte{1};
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                CHECK(retained->winners.size() == result->winners.size());
            }
            SECTION("byte budget and invalid results do not create entries") {
                auto limited = QueryResultCache::Create(scene->View().RuntimeId(), {1, 256, 3});
                REQUIRE(limited.HasValue());
                auto small = std::move(limited).Value();
                ExpectError(small.Store(scene->View(), inputs, authority, key, *result, tick), AIErrors::EnvironmentQueryLimitExceeded);
                auto partial = *result;
                partial.status = QueryExecutionStatus::PartialSuccess;
                CHECK_FALSE(cache.Store(scene->View(), inputs, authority, key, partial, tick).Value());
                CHECK_FALSE(small.Lookup(scene->View(), inputs, authority, key, tick));
            }
            SECTION("point-only sharing removes the producing agent identity") {
                key.sharedPointData = true;
                REQUIRE(cache.Store(scene->View(), inputs, authority, key, *result, tick).Value());
                auto other = inputs;
                ++other.agent.slot.generation;
                const auto shared = cache.Lookup(scene->View(), other, authority, key, tick);
                REQUIRE(shared);
                CHECK_FALSE(shared->agent.IsValid());
            }
            SECTION("retired winner identities miss without changing a retained snapshot") {
                const auto entity = Source(scene->View()).querier;
                auto actorResult = *result;
                REQUIRE_FALSE(actorResult.winners.empty());
                actorResult.winners.front().item = entity;
                key.sharedPointData = true;
                CHECK_FALSE(cache.Store(scene->View(), inputs, authority, key, actorResult, tick).Value());
                key.sharedPointData = false;
                REQUIRE(cache.Store(scene->View(), inputs, authority, key, actorResult, tick).Value());
                const auto actors = cache.Lookup(scene->View(), inputs, authority, key, tick);
                REQUIRE(actors);
                Runtime::SceneCommandBuffer unload;
                unload.Destroy(entity);
                REQUIRE(scene->Commit(unload).HasValue());
                CHECK_FALSE(cache.Lookup(scene->View(), inputs, authority, key, tick));
                CHECK(std::get<Runtime::EntityRef>(actors->winners.front().item) == entity);
            }
        }

        TEST_CASE("Deterministic EQS uses real reserved owner scheduling and publishes the same semantic result tick",
                  "[unit][ai][eqs][execution][deterministic]") {
            for (const std::uint32_t workers : {0U, 1U}) {
                auto scene = MakeScene();
                ExecutionProbe probe;
                auto query = MakeExecution(probe, *scene, false, true);
                JobSystem jobs{{.workerCount = workers}};
                auto scheduler = AiTaskScheduler::Create(jobs, MakeIdentity<AiRuntimeIncarnation>(7),
                                                         {.maximumAgents = 1,
                                                          .evaluationsPerTick = 1,
                                                          .workUnitsPerTick = 10,
                                                          .commandsPerTick = 1,
                                                          .workerSubmissionsPerTick = 1,
                                                          .pendingWorkers = 1});
                REQUIRE(scheduler.HasValue());
                auto decision = std::make_shared<ScheduledQuery>(*query);
                REQUIRE(scheduler.Value()
                            ->Register({MakeIdentity<AiRuntimeIncarnation>(7), {1, 1}}, MakeIdentity<AgentId>(1),
                                       {.workUnitsPerTick = 10, .commandsPerTick = 1}, std::make_shared<int>(1), decision)
                            .HasValue());
                AiWorkBudget forbidden{100};
                query->Advance(4, {forbidden, forbidden, forbidden});
                CHECK(probe.generated == 0);
                for (std::uint64_t tick = 5; tick <= 7; ++tick) {
                    const auto terminal = query->Sync(tick);
                    CHECK(terminal == (tick == 7));
                    REQUIRE(scheduler.Value()->EvaluateAtDecision(tick).HasValue());
                    REQUIRE(scheduler.Value()->CommitAtIntentDispatch(tick).HasValue());
                }
                REQUIRE(query->Published());
                CHECK(query->Published()->mode == QueryExecutionMode::DeterministicWorkUnits);
                CHECK(query->Published()->publicationTick == 7);
                CHECK(query->Published()->status == QueryExecutionStatus::Completed);
                REQUIRE(query->Published()->winners.size() == 2);
                CHECK(query->Published()->winners[0].score.itemIndex == 1);
                CHECK(probe.generated == 1);
                CHECK(probe.attempts == 2);
            }
        }

        TEST_CASE("Deterministic EQS rejects skipped owner ticks and over-budget kernels without consuming late facts",
                  "[unit][ai][eqs][execution][deterministic]") {
            for (const bool overrun : {false, true}) {
                auto scene = MakeScene();
                ExecutionProbe probe;
                probe.overrun = overrun;
                auto query = MakeExecution(probe, *scene, false, true);
                CHECK_FALSE(query->Sync(5));
                AiWorkBudget budget{10};
                query->Advance(5, {budget, budget, budget});
                const std::uint64_t terminalTick = overrun ? 6 : 7;
                REQUIRE(query->Sync(terminalTick));
                REQUIRE(query->Published());
                CHECK(query->Published()->status == QueryExecutionStatus::Failed);
                REQUIRE(query->Published()->error);
                CHECK(query->Published()->error->code.Value() ==
                      (overrun ? AIErrors::SchedulerBudgetExhausted.code.Value() : AIErrors::SchedulerPhaseInvalid.code.Value()));
                CHECK(query->Published()->winners.empty());
                CHECK(query->Published()->publicationTick == terminalTick);
                CHECK(probe.attempts == 1);
                CHECK_FALSE(query->Sync(8));
            }
        }

        TEST_CASE("Deterministic EQS discards generation output that exceeds its declared kernel allowance",
                  "[unit][ai][eqs][execution][deterministic]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            probe.generationOverrun = true;
            auto query = MakeExecution(probe, *scene, false, true);
            CHECK_FALSE(query->Sync(5));
            AiWorkBudget budget{10};
            query->Advance(5, {budget, budget, budget});
            REQUIRE(query->Sync(6));
            REQUIRE(query->Published()->error);
            CHECK(query->Published()->error->code.Value() == AIErrors::SchedulerBudgetExhausted.code.Value());
            CHECK(query->Published()->status == QueryExecutionStatus::Failed);
            CHECK(query->Published()->winners.empty());
            CHECK(probe.attempts == 0);
        }

        TEST_CASE("Deterministic EQS cannot silently skip a reserved decision phase", "[unit][ai][eqs][execution][deterministic]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene, false, true);
            CHECK_FALSE(query->Sync(5));
            REQUIRE(query->Sync(6));
            REQUIRE(query->Published());
            CHECK(query->Published()->status == QueryExecutionStatus::Failed);
            REQUIRE(query->Published()->error);
            CHECK(query->Published()->error->code.Value() == AIErrors::SchedulerPhaseInvalid.code.Value());
            CHECK(probe.generated == 0);
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
            CancellationSource owner;
            auto query = MakeExecution(probe, *scene, false, false, nullptr, {}, owner.Token());
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
            SECTION("explicit cancellation") {
                query->Cancel();
                query->Cancel();
                expected = QueryExecutionStatus::Cancelled;
            }
            SECTION("scene owner retirement cancels before the next provider poll") {
                owner.RequestCancellation();
                CHECK_FALSE(query->InputsAreCurrent());
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
            CHECK_FALSE(query->ReadDebugSnapshot(scene->View()));
        }

        TEST_CASE("EQS debug explains a sampled stage and ranks winners independently of sampling", "[unit][ai][eqs][debug]") {
            auto scene = MakeScene();
            ExecutionProbe probe;
            auto query = MakeExecution(probe, *scene, false, false, nullptr, {1, true});
            AiWorkBudget first{9}, second{2};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK_FALSE(query->Sync(6));
            query->Advance(6, {second, second, second});
            REQUIRE(query->Sync(7));
            const auto captured = query->ReadDebugSnapshot(scene->View());
            REQUIRE(captured);
            CHECK(captured->terminalStatus == QueryExecutionStatus::Completed);
            CHECK(captured->evaluated == 2);
            REQUIRE(captured->candidates.size() == 1);
            CHECK(captured->candidates[0].index == 0);
            REQUIRE(captured->candidates[0].point);
            CHECK(captured->candidates[0].point->Millimeters()[0] == 1);
            REQUIRE(captured->stages.size() == 1);
            CHECK(captured->stages[0].candidateIndex == 0);
            CHECK(captured->stages[0].score.contribution == 0.5);
            CHECK(captured->winnerCount == 2);
            REQUIRE(captured->winners.size() == 1);
            CHECK(captured->winners[0].itemIndex == 1);
            CHECK(captured->winners[0].scoreKey == QueryScoringPlan::ScoreKeyScale);

            SECTION("provider generation changed") {
                probe.current = false;
            }
            SECTION("the same Scene published a structural replacement") {
                const auto prior = scene->View().StructuralRevision();
                Runtime::SceneCommandBuffer commands;
                REQUIRE(commands.Create({}).IsValid());
                REQUIRE(scene->Commit(std::move(commands)).HasValue());
                CHECK(scene->View().StructuralRevision() != prior);
            }
            const auto stale = query->ReadDebugSnapshot(scene->View());
            REQUIRE(stale);
            CHECK(stale->stale);
            CHECK_FALSE(stale->candidates[0].point);
        }

        TEST_CASE("EQS debug samples large sets evenly and redacts provider values", "[unit][ai][eqs][debug]") {
            auto scene = MakeScene();
            ExecutionProbe probe{.pointCount = 129, .filter = false};
            auto query = MakeExecution(probe, *scene, false, false, nullptr, {3});
            AiWorkBudget first{2'000};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            CHECK_FALSE(query->Sync(6));
            const auto captured = query->ReadDebugSnapshot(scene->View());
            REQUIRE(captured);
            CHECK(captured->generated == 129);
            CHECK(captured->evaluated == 1);
            REQUIRE(captured->candidates.size() == 3);
            CHECK(captured->candidates[0].index == 0);
            CHECK(captured->candidates[1].index == 64);
            CHECK(captured->candidates[2].index == 128);
            for (const auto &candidate : captured->candidates)
                CHECK_FALSE(candidate.point);
            REQUIRE(captured->stages.size() == 1);
            CHECK(captured->stages[0].score.status == QueryScoreStatus::Filtered);
            CHECK(captured->stages[0].score.contribution == 0);
            CHECK(captured->winners.empty());
        }

        TEST_CASE("EQS debug detects a replaced actor from the actual scene generation", "[unit][ai][eqs][debug]") {
            auto scene = MakeScene();
            const auto actor = scene->View().Find({2});
            REQUIRE(actor);
            ExecutionProbe probe{.actor = *actor};
            auto query = MakeExecution(probe, *scene, false, false, nullptr, {2, true});
            AiWorkBudget first{9};
            CHECK_FALSE(query->Sync(5));
            query->Advance(5, {first, first, first});
            REQUIRE(query->Sync(6));
            const auto current = query->ReadDebugSnapshot(scene->View());
            REQUIRE(current);
            CHECK_FALSE(current->stale);
            REQUIRE(current->candidates.size() == 1);
            CHECK(current->candidates[0].actor);
            CHECK_FALSE(current->candidates[0].point);

            Runtime::SceneCommandBuffer commands;
            commands.Destroy(*actor);
            REQUIRE(commands.Create({}).IsValid());
            const auto committed = scene->Commit(commands);
            REQUIRE(committed.HasValue());
            REQUIRE(committed.Value().created.size() == 1);
            const auto replacement = committed.Value().created[0].entity;
            CHECK(replacement != *actor);
            const auto stale = query->ReadDebugSnapshot(scene->View());
            REQUIRE(stale);
            CHECK(stale->stale);
            CHECK_FALSE(stale->candidates[0].point);
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

        TEST_CASE("EQS isolates generator admission and poll exceptions from other queries", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            for (const auto point : {SourceFailure::Generation, SourceFailure::Admission, SourceFailure::Poll, SourceFailure::UnknownPoll,
                                     SourceFailure::TypedAdmission}) {
                ExecutionProbe failing{.failure = point}, healthy;
                auto rejected = MakeExecution(failing, *scene);
                auto accepted = MakeExecution(healthy, *scene);
                AiWorkBudget firstFailure{9}, firstHealthy{9}, secondHealthy{2};
                REQUIRE_FALSE(rejected->Sync(5));
                REQUIRE_FALSE(accepted->Sync(5));
                REQUIRE_NOTHROW(rejected->Advance(5, {firstFailure, firstFailure, firstFailure}));
                accepted->Advance(5, {firstHealthy, firstHealthy, firstHealthy});
                REQUIRE(rejected->Sync(6));
                REQUIRE(rejected->Published());
                CHECK(rejected->Published()->status == QueryExecutionStatus::Failed);
                CHECK(rejected->Published()->winners.empty());
                REQUIRE(rejected->Published()->error);
                const auto &expected =
                    point == SourceFailure::TypedAdmission ? AIErrors::CapabilityUnavailable : AIErrors::EnvironmentQueryProviderFailed;
                CHECK(rejected->Published()->error->code.Value() == expected.code.Value());
                CHECK(rejected->Published()->error->domain.Value() == expected.domain.Value());
                CHECK(failing.cancelled == ((point == SourceFailure::Poll || point == SourceFailure::UnknownPoll) ? 1 : 0));
                REQUIRE_FALSE(accepted->Sync(6));
                accepted->Advance(6, {secondHealthy, secondHealthy, secondHealthy});
                REQUIRE(accepted->Sync(7));
                CHECK(accepted->Published()->status == QueryExecutionStatus::Completed);
                CHECK(accepted->Published()->winners.size() == 2);
                CHECK_FALSE(rejected->Sync(7));
            }
        }

        TEST_CASE("EQS diagnostic allocation failure closes pending work and publishes one typed terminal", "[unit][ai][eqs][execution]") {
            auto scene = MakeScene();
            for (const auto point : {SourceFailure::DiagnosticPoll, SourceFailure::UnknownPoll}) {
                ExecutionProbe probe{.failure = point};
                auto query = MakeExecution(probe, *scene);
                AiWorkBudget first{9};
                REQUIRE_FALSE(query->Sync(5));
                query->Advance(5, {first, first, first});
                REQUIRE(probe.attempts == 1);
                const bool published = [&] {
                    Tests::AllocationProbe::ScopedFailure allocation;
                    return query->Sync(6);
                }();
                REQUIRE(published);
                const auto *result = query->Published();
                REQUIRE(result);
                CHECK(result->status == QueryExecutionStatus::Failed);
                CHECK(result->winners.empty());
                REQUIRE(result->error);
                CHECK(result->error->code.Value() == AIErrors::EnvironmentQueryProviderFailed.code.Value());
                CHECK(result->error->message.empty());
                CHECK(probe.cancelled == 1);
                query->Cancel();
                CHECK_FALSE(query->Sync(7));
                query.reset();
                CHECK(probe.cancelled == 1);
            }
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

    }  // namespace
}  // namespace Horo::AI
