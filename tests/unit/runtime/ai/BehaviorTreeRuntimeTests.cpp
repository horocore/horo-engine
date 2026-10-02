#include "AiTestSupport.h"
#include "Horo/AI/BehaviorTreeRuntime.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
    // Test-thread-only counters/fault injection; no worker state or production allocation policy is changed.
    thread_local std::size_t allocations{};
    thread_local bool failNextAllocation{};

    void *Allocate(const std::size_t size) {
        ++allocations;
        if (std::exchange(failNextAllocation, false))
            throw std::bad_alloc{};
        if (void *storage = std::malloc(size == 0 ? 1 : size))
            return storage;
        throw std::bad_alloc{};
    }
}  // namespace

void *operator new(const std::size_t size) {
    return Allocate(size);
}

void *operator new[](const std::size_t size) {
    return Allocate(size);
}

void operator delete(void *storage) noexcept {
    std::free(storage);
}

void operator delete[](void *storage) noexcept {
    std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept {
    std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept {
    std::free(storage);
}

namespace Horo::AI {
    namespace {
        using TestSupport::ExpectError;
        using TestSupport::MakeIdentity;

        DecisionNodeId Id(const std::uint64_t value) {
            return MakeIdentity<DecisionNodeId>(value);
        }

        BehaviorTreeExecutionNode Node(const std::uint64_t id, const BehaviorTreeOperation operation,
                                       const std::initializer_list<std::uint64_t> children = {}) {
            BehaviorTreeExecutionNode node{.id = Id(id), .operation = operation};
            for (const auto child : children)
                node.children.push_back(Id(child));
            if (operation == BehaviorTreeOperation::Task)
                node.task = MakeIdentity<TaskId>(id);
            return node;
        }

        struct Probe final {
            static constexpr std::size_t Capacity = BehaviorTreeExecutionLimits::HardNodes + 1;
            std::array<AiTaskState, Capacity> outcome{};
            std::array<std::size_t, Capacity> starts{}, resumes{}, aborts{}, cleanups{}, checks{}, services{};
            std::array<TaskHandle, Capacity> handles{};
            std::array<std::optional<AiTaskTerminalResult>, Capacity> terminals{};
            std::array<std::uint64_t, Capacity * 4> order{};
            std::size_t orderCount{};
            std::size_t destroyed{};
            std::uint64_t failTask{}, failService{}, failCheck{};
            bool invalidOutput{};

            Probe() {
                outcome.fill(AiTaskState::Succeeded);
            }
        };

        class Executor final : public IBehaviorTreeExecutor {
        public:
            explicit Executor(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}

            ~Executor() override {
                ++probe_->destroyed;
            }

            Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context,
                                      const AiTaskOperationContext &operation) noexcept override {
                const auto id = context.node.Value();
                ++probe_->starts[id];
                probe_->handles[id] = operation.task;
                probe_->order[probe_->orderCount++] = id;
                if (id == probe_->failTask)
                    return Result<AiTaskState>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                return Result<AiTaskState>::Success(probe_->invalidOutput ? AiTaskState::Idle : probe_->outcome[id]);
            }

            Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &) noexcept override {
                ++probe_->resumes[context.node.Value()];
                return Result<AiTaskState>::Success(probe_->outcome[context.node.Value()]);
            }

            void Abort(const AiTaskOperationContext &operation, const AiTaskCancellationReason) noexcept override {
                ++probe_->aborts[operation.taskDefinition.Value()];
            }

            void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept override {
                ++probe_->cleanups[operation.taskDefinition.Value()];
                probe_->terminals[operation.taskDefinition.Value()] = result;
            }

            Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept override {
                ++probe_->checks[context.node.Value()];
                if (context.node.Value() == probe_->failCheck)
                    return Result<bool>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                if (context.bindings.size() != 1)
                    return Result<bool>::Failure(MakeError(AIErrors::DecisionAssetBindingMissing));
                auto value = context.blackboard.Read(context.bindings[0].key);
                if (value.HasError())
                    return Result<bool>::Failure(value.ErrorValue());
                return Result<bool>::Success(std::get<bool>(std::get<BlackboardScalarValue>(*value.Value())));
            }

            Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept override {
                ++probe_->services[context.node.Value()];
                if (context.node.Value() == probe_->failService)
                    return Result<void>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                return Result<void>::Success();
            }

        private:
            std::shared_ptr<Probe> probe_;
        };

        struct Fixture final {
            AgentHandle agent{MakeIdentity<AiRuntimeIncarnation>(1), {4, 1}};
            std::shared_ptr<const BlackboardSchema> schema;
            std::shared_ptr<Probe> probe{std::make_shared<Probe>()};
            std::unique_ptr<BlackboardInstance> blackboard;
            std::vector<BehaviorTreeExecutionNode> nodes;
            std::vector<BehaviorTreeExecutionService> services;

            explicit Fixture(std::vector<BehaviorTreeExecutionNode> source, std::vector<BehaviorTreeExecutionService> attachments = {})
                : nodes(std::move(source)), services(std::move(attachments)) {
                auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(7),
                                                           .keys = {{.key = MakeIdentity<BlackboardKeyId>(10),
                                                                     .kind = BlackboardValueKind::Boolean,
                                                                     .cardinality = BlackboardValueCardinality::Scalar,
                                                                     .maximumCollectionElements = 1,
                                                                     .presence = BlackboardKeyPresence::Required,
                                                                     .access = BlackboardKeyAccess::ReadWrite,
                                                                     .defaultValue = BlackboardValue{BlackboardScalarValue{true}}}}});
                REQUIRE(captured.HasValue());
                schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
                auto storage = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
                REQUIRE(storage.HasValue());
                blackboard = std::move(storage).Value();
            }

            std::shared_ptr<const DecisionAssetPlan> Bindings(const DecisionPlanKind kind = DecisionPlanKind::BehaviorTree) const {
                DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(100),
                                              .kind = kind,
                                              .blackboardSchema = schema->Identity()};
                for (const auto &node : nodes)
                    asset.nodes.push_back({.id = node.id, .type = MakeIdentity<DecisionNodeTypeId>(101)});
                for (const auto &service : services)
                    asset.nodes.push_back({.id = service.id, .type = MakeIdentity<DecisionNodeTypeId>(101)});
                const std::array descriptors{
                    DecisionNodeDescriptor{.type = MakeIdentity<DecisionNodeTypeId>(101),
                                           .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(501), 1},
                                           .requirements = {
                                               {.key = MakeIdentity<BlackboardKeyId>(10), .kind = BlackboardValueKind::Boolean}}}};
                auto compiled = DecisionAssetCompiler::Compile(asset, {}, descriptors, std::array{schema});
                REQUIRE(compiled.HasValue());
                REQUIRE(compiled.Value().IsValid());
                return compiled.Value().plan;
            }

            std::shared_ptr<const BehaviorTreeExecutionPlan> Plan() const {
                auto compiled = BehaviorTreeExecutionPlan::Compile(Bindings(), Id(1), nodes, services);
                REQUIRE(compiled.HasValue());
                return compiled.Value();
            }

            std::unique_ptr<BehaviorTreeInstance> Instance(const std::uint32_t generation = 1,
                                                           const CancellationToken cancellation = {}) const {
                const BehaviorTreeInstanceBinding binding{agent,
                                                          cancellation,
                                                          100,
                                                          generation,
                                                          {agent, schema->Identity(), schema->Version(), 1, 1}};
                auto result = BehaviorTreeInstance::Create(Plan(), binding, std::make_unique<Executor>(probe));
                REQUIRE(result.HasValue());
                return std::move(result).Value();
            }

            void SetCondition(const bool value) {
                auto batch = blackboard->BeginWriteBatch();
                REQUIRE(batch.HasValue());
                REQUIRE(batch.Value().Stage({MakeIdentity<BlackboardKeyId>(10), BlackboardValue{BlackboardScalarValue{value}}}).HasValue());
                REQUIRE(blackboard->CommitAtBlackboardSync(std::move(batch).Value()).HasValue());
            }

            AiTaskState Evaluate(BehaviorTreeInstance &instance, const std::uint64_t tick,
                                 const AiTaskResumeReason reason = AiTaskResumeReason::FixedTick) const {
                auto snapshot = blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                auto result = instance.Evaluate(tick, snapshot.Value(), agent, reason);
                REQUIRE(result.HasValue());
                return result.Value();
            }
        };

        TEST_CASE("Behavior tree sequence preserves declared order and resumes only the active task", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(4, BehaviorTreeOperation::Task), Node(1, BehaviorTreeOperation::Sequence, {3, 2, 4}),
                             Node(2, BehaviorTreeOperation::Task), Node(3, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[2] = AiTaskState::Running;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.probe->order[0] == 3);
            CHECK(fixture.probe->order[1] == 2);
            CHECK(fixture.probe->starts[4] == 0);
            CHECK(fixture.Evaluate(*instance, 1, AiTaskResumeReason::Event) == AiTaskState::Running);
            fixture.probe->outcome[2] = AiTaskState::Succeeded;
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Succeeded);
            CHECK(fixture.probe->resumes[2] == 2);
            CHECK(fixture.probe->starts[3] == 1);
            CHECK(fixture.probe->order[2] == 4);
            CHECK(fixture.Evaluate(*instance, 3) == AiTaskState::Succeeded);
            CHECK(fixture.probe->cleanups[2] == 1);
            CHECK(fixture.probe->aborts[2] == 0);
        }

        TEST_CASE("Behavior tree selector falls through failures and sequence stops at failure", "[unit][ai][bt_runtime]") {
            for (const auto operation : {BehaviorTreeOperation::Selector, BehaviorTreeOperation::Sequence}) {
                Fixture fixture({Node(1, operation, {3, 2}), Node(2, BehaviorTreeOperation::Task), Node(3, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[3] = AiTaskState::Failed;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) ==
                      (operation == BehaviorTreeOperation::Selector ? AiTaskState::Succeeded : AiTaskState::Failed));
                CHECK(fixture.probe->starts[2] == (operation == BehaviorTreeOperation::Selector ? 1 : 0));
            }
        }

        TEST_CASE("Parallel policies step each branch once in stable order and cancel terminal losers", "[unit][ai][bt_runtime]") {
            for (const auto policy : {BehaviorTreeParallelPolicy::RequireOneSuccess, BehaviorTreeParallelPolicy::RequireAllSuccess,
                                      BehaviorTreeParallelPolicy::RequireAllComplete, BehaviorTreeParallelPolicy::StopOthersOnFailure}) {
                auto root = Node(1, BehaviorTreeOperation::Parallel, {3, 2, 4});
                root.parallel = policy;
                Fixture fixture({root, Node(2, BehaviorTreeOperation::Task), Node(3, BehaviorTreeOperation::Task),
                                 Node(4, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[2] = AiTaskState::Running;
                fixture.probe->outcome[4] = AiTaskState::Failed;
                auto instance = fixture.Instance();
                const bool stops =
                    policy == BehaviorTreeParallelPolicy::RequireOneSuccess || policy == BehaviorTreeParallelPolicy::StopOthersOnFailure;
                const auto first = fixture.Evaluate(*instance, 1);
                CHECK(first == (policy == BehaviorTreeParallelPolicy::RequireOneSuccess     ? AiTaskState::Succeeded
                                : policy == BehaviorTreeParallelPolicy::StopOthersOnFailure ? AiTaskState::Failed
                                                                                            : AiTaskState::Running));
                CHECK(fixture.probe->order[0] == 3);
                CHECK(fixture.probe->order[1] == 2);
                CHECK(fixture.probe->order[2] == 4);
                CHECK(fixture.probe->aborts[2] == (stops ? 1 : 0));
                fixture.probe->outcome[2] = AiTaskState::Succeeded;
                const auto last = fixture.Evaluate(*instance, 2);
                CHECK(last ==
                      (policy == BehaviorTreeParallelPolicy::RequireAllComplete || policy == BehaviorTreeParallelPolicy::RequireOneSuccess
                           ? AiTaskState::Succeeded
                           : AiTaskState::Failed));
                CHECK(fixture.probe->starts[3] == 1);
                CHECK(fixture.probe->cleanups[2] == 1);
                instance->Shutdown();
                CHECK(fixture.probe->aborts[2] == (stops ? 1 : 0));
            }
        }

        TEST_CASE("Subtree abort cancels every nested running descendant exactly once", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Parallel, {2, 3}), Node(2, BehaviorTreeOperation::Sequence, {4}),
                             Node(3, BehaviorTreeOperation::Inverter, {5}), Node(4, BehaviorTreeOperation::Task),
                             Node(5, BehaviorTreeOperation::Task)});
            fixture.probe->outcome.fill(AiTaskState::Running);
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            REQUIRE(instance->AbortSubtree(Id(1)).HasValue());
            REQUIRE(instance->AbortSubtree(Id(1)).HasValue());
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Cancelled);
            instance->Shutdown();
            instance.reset();
            for (const auto id : {4, 5}) {
                CHECK(fixture.probe->aborts[id] == 1);
                CHECK(fixture.probe->cleanups[id] == 1);
                CHECK(fixture.probe->terminals[id]->state == AiTaskState::Cancelled);
            }
            CHECK(fixture.probe->destroyed == 1);
        }

        TEST_CASE("Decorator self abort modes recheck frozen blackboard and cancel a running child", "[unit][ai][bt_runtime]") {
            for (const auto mode : {BehaviorTreeAbortMode::None, BehaviorTreeAbortMode::Self, BehaviorTreeAbortMode::LowerPriority,
                                    BehaviorTreeAbortMode::Both}) {
                auto check = Node(2, BehaviorTreeOperation::BlackboardCheck, {4});
                check.abort = mode;
                Fixture fixture({Node(1, BehaviorTreeOperation::Selector, {2, 3}), check, Node(3, BehaviorTreeOperation::Task),
                                 Node(4, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[4] = AiTaskState::Running;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
                fixture.SetCondition(false);
                const bool self = mode == BehaviorTreeAbortMode::Self || mode == BehaviorTreeAbortMode::Both;
                CHECK(fixture.Evaluate(*instance, 2) == (self ? AiTaskState::Succeeded : AiTaskState::Running));
                CHECK(fixture.probe->aborts[4] == (self ? 1 : 0));
                CHECK(fixture.probe->checks[2] == (self ? 2 : 1));
            }
        }

        TEST_CASE("Lower priority abort reselects the first newly eligible branch", "[unit][ai][bt_runtime]") {
            for (const auto mode : {BehaviorTreeAbortMode::None, BehaviorTreeAbortMode::Self, BehaviorTreeAbortMode::LowerPriority,
                                    BehaviorTreeAbortMode::Both}) {
                auto check = Node(2, BehaviorTreeOperation::BlackboardCheck, {4});
                check.abort = mode;
                Fixture fixture({Node(1, BehaviorTreeOperation::Selector, {2, 3}), check, Node(3, BehaviorTreeOperation::Task),
                                 Node(4, BehaviorTreeOperation::Task)});
                fixture.SetCondition(false);
                fixture.probe->outcome[3] = AiTaskState::Running;
                auto instance = fixture.Instance();
                REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
                fixture.SetCondition(true);
                const bool lower = mode == BehaviorTreeAbortMode::LowerPriority || mode == BehaviorTreeAbortMode::Both;
                CHECK(fixture.Evaluate(*instance, 2) == (lower ? AiTaskState::Succeeded : AiTaskState::Running));
                CHECK(fixture.probe->aborts[3] == (lower ? 1 : 0));
                CHECK(fixture.probe->starts[4] == (lower ? 1 : 0));
                CHECK(fixture.probe->checks[2] == (lower ? 2 : 1));
            }
        }

        TEST_CASE("Inverter preserves running and cancellation and flips terminal success or failure", "[unit][ai][bt_runtime]") {
            for (const auto outcome : {AiTaskState::Running, AiTaskState::Succeeded, AiTaskState::Failed, AiTaskState::Cancelled}) {
                Fixture fixture({Node(1, BehaviorTreeOperation::Inverter, {2}), Node(2, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[2] = outcome;
                auto instance = fixture.Instance();
                CHECK(fixture.Evaluate(*instance, 1) == (outcome == AiTaskState::Succeeded ? AiTaskState::Failed
                                                         : outcome == AiTaskState::Failed  ? AiTaskState::Succeeded
                                                                                           : outcome));
            }
        }

        TEST_CASE("Loops yield once per iteration and issue fresh nonwrapping task generations", "[unit][ai][bt_runtime]") {
            auto loop = Node(1, BehaviorTreeOperation::Loop, {2});
            loop.iterations = 3;
            Fixture fixture({loop, Node(2, BehaviorTreeOperation::Task)});
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            const auto first = fixture.probe->handles[2];
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.probe->handles[2] != first);
            CHECK(fixture.Evaluate(*instance, 3) == AiTaskState::Succeeded);
            CHECK(fixture.probe->starts[2] == 3);
            CHECK(fixture.probe->cleanups[2] == 3);
        }

        TEST_CASE("Cooldown and time limit use only the declared fixed-tick clock", "[unit][ai][bt_runtime]") {
            auto cooldown = Node(1, BehaviorTreeOperation::Cooldown, {2});
            cooldown.durationTicks = 3;
            Fixture fixture({cooldown, Node(2, BehaviorTreeOperation::Task)});
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Succeeded);
            REQUIRE(instance->Restart().HasValue());
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Failed);
            CHECK(fixture.probe->starts[2] == 1);
            REQUIRE(instance->Restart().HasValue());
            CHECK(fixture.Evaluate(*instance, 4) == AiTaskState::Succeeded);
            auto limit = Node(1, BehaviorTreeOperation::TimeLimit, {2});
            limit.durationTicks = 2;
            Fixture timed({limit, Node(2, BehaviorTreeOperation::Task)});
            timed.probe->outcome[2] = AiTaskState::Running;
            auto running = timed.Instance();
            CHECK(timed.Evaluate(*running, 10) == AiTaskState::Running);
            CHECK(timed.Evaluate(*running, 10, AiTaskResumeReason::Event) == AiTaskState::Running);
            CHECK(timed.Evaluate(*running, 12) == AiTaskState::Failed);
            CHECK(timed.probe->aborts[2] == 1);
            CHECK(timed.probe->resumes[2] == 1);
        }

        TEST_CASE("Periodic and reactive services run only on active subtrees at declared boundaries", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2, 3}), Node(2, BehaviorTreeOperation::Task),
                             Node(3, BehaviorTreeOperation::Task)},
                            {{Id(4), Id(1), BehaviorTreeServiceMode::Periodic, 3},
                             {Id(5), Id(1), BehaviorTreeServiceMode::Reactive, 1},
                             {Id(6), Id(3), BehaviorTreeServiceMode::Periodic, 1}});
            fixture.probe->outcome[2] = AiTaskState::Running;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.probe->services[4] == 1);
            CHECK(fixture.probe->services[5] == 1);
            CHECK(fixture.probe->services[6] == 0);
            fixture.SetCondition(false);
            CHECK(fixture.Evaluate(*instance, 2, AiTaskResumeReason::Event) == AiTaskState::Running);
            CHECK(fixture.probe->services[5] == 2);
            fixture.probe->outcome[2] = AiTaskState::Succeeded;
            CHECK(fixture.Evaluate(*instance, 4) == AiTaskState::Succeeded);
            CHECK(fixture.probe->services[4] == 2);
            CHECK(fixture.probe->services[6] == 1);
            CHECK(fixture.Evaluate(*instance, 5) == AiTaskState::Succeeded);
            CHECK(fixture.probe->services[4] == 2);
        }

        TEST_CASE("Reload preserves compatible tasks and rejects null while incompatible topology aborts", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2}), Node(2, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[2] = AiTaskState::Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            const auto handle = fixture.probe->handles[2];
            auto retained = instance->Replace(fixture.Plan());
            REQUIRE(retained.HasValue());
            CHECK(retained.Value());
            CHECK(fixture.probe->aborts[2] == 0);
            ExpectError(instance->Replace(nullptr), AIErrors::DecisionAssetActivationInvalid);
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.probe->handles[2] == handle);
            fixture.nodes[0].operation = BehaviorTreeOperation::Selector;
            auto restarted = instance->Replace(fixture.Plan());
            REQUIRE(restarted.HasValue());
            CHECK_FALSE(restarted.Value());
            CHECK(instance->RootStatus() == AiTaskState::Idle);
            CHECK(fixture.probe->aborts[2] == 1);
            CHECK(fixture.Evaluate(*instance, 3) == AiTaskState::Running);
            CHECK(fixture.probe->handles[2] != handle);
        }

        TEST_CASE("Generation retirement cancellation and shutdown prevent any later callback", "[unit][ai][bt_runtime]") {
            for (const bool generation : {true, false}) {
                Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
                fixture.probe->outcome[1] = AiTaskState::Running;
                CancellationSource cancellation;
                auto instance = fixture.Instance(1, cancellation.Token());
                REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
                auto snapshot = fixture.blackboard->Snapshot();
                REQUIRE(snapshot.HasValue());
                auto active = fixture.agent;
                if (generation)
                    ++active.slot.generation;
                else
                    cancellation.RequestCancellation();
                auto result = instance->Evaluate(2, snapshot.Value(), active);
                REQUIRE(result.HasValue());
                CHECK(result.Value() == AiTaskState::Cancelled);
                CHECK(fixture.probe->terminals[1]->cancellationReason ==
                      (generation ? AiTaskCancellationReason::AgentGenerationRetired : AiTaskCancellationReason::ContextCancelled));
                ExpectError(instance->Evaluate(3, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
                instance->Shutdown();
                CHECK(fixture.probe->aborts[1] == 1);
                CHECK(fixture.probe->cleanups[1] == 1);
                CHECK(fixture.probe->resumes[1] == 0);
            }
        }

        TEST_CASE("Precancelled trees never start tasks and destructors abort running work", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            CancellationSource cancellation;
            cancellation.RequestCancellation();
            auto cancelled = fixture.Instance(1, cancellation.Token());
            CHECK(fixture.Evaluate(*cancelled, 1) == AiTaskState::Cancelled);
            CHECK(fixture.probe->starts[1] == 0);
            CHECK(fixture.probe->aborts[1] == 0);
            fixture.probe->outcome[1] = AiTaskState::Running;
            auto running = fixture.Instance();
            REQUIRE(fixture.Evaluate(*running, 1) == AiTaskState::Running);
            running.reset();
            CHECK(fixture.probe->terminals[1]->cancellationReason == AiTaskCancellationReason::OwnerShutdown);
            CHECK(fixture.probe->aborts[1] == 1);
            CHECK(fixture.probe->cleanups[1] == 1);
        }

        TEST_CASE("Task failures retain the original typed cause and invalid output is terminal", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->failTask = 1;
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Failed);
            REQUIRE(fixture.probe->terminals[1]->failure.has_value());
            CHECK(fixture.probe->terminals[1]->failure->cause.code == AIErrors::RuntimeUnavailable.code);
            CHECK(fixture.probe->cleanups[1] == 1);
            REQUIRE(instance->Restart().HasValue());
            fixture.probe->failTask = 0;
            fixture.probe->invalidOutput = true;
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Failed);
            CHECK(fixture.probe->terminals[1]->failure->kind == AiTaskFailureKind::InvalidOutput);
        }

        TEST_CASE("Provider service failure aborts previously running branches without losing the error", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Parallel, {2, 3}), Node(2, BehaviorTreeOperation::Task),
                             Node(3, BehaviorTreeOperation::Task)},
                            {{Id(4), Id(3), BehaviorTreeServiceMode::Periodic, 1}});
            fixture.probe->outcome[2] = AiTaskState::Running;
            fixture.probe->failService = 4;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::RuntimeUnavailable);
            CHECK(fixture.probe->aborts[2] == 1);
            CHECK(fixture.probe->cleanups[2] == 1);
            CHECK(fixture.probe->starts[3] == 0);
        }

        TEST_CASE("Task generation exhaustion cannot wrap or admit replacement work", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2, 3}), Node(2, BehaviorTreeOperation::Task),
                             Node(3, BehaviorTreeOperation::Task)});
            auto instance = fixture.Instance(std::numeric_limits<std::uint32_t>::max());
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::GenerationExhausted);
            CHECK(fixture.probe->starts[2] == 1);
            CHECK(fixture.probe->starts[3] == 0);
            CHECK(fixture.probe->cleanups[2] == 1);
        }

        TEST_CASE("Invalid clocks and stale blackboards reject evaluation without touching tasks", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = AiTaskState::Running;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(0, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
            REQUIRE(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            ExpectError(instance->Evaluate(1, snapshot.Value(), fixture.agent), AIErrors::TaskTransitionInvalid);
            REQUIRE(fixture.blackboard->TeardownAtBlackboardSync().HasValue());
            ExpectError(instance->Evaluate(3, snapshot.Value(), fixture.agent), AIErrors::BlackboardInstanceStale);
            CHECK(fixture.probe->starts[1] == 1);
            CHECK(fixture.probe->resumes[1] == 0);
        }

        TEST_CASE("The maximum-depth tree executes iteratively and lower project limits reject it", "[unit][ai][bt_runtime]") {
            std::vector<BehaviorTreeExecutionNode> nodes;
            for (std::size_t id = 1; id < BehaviorTreeExecutionLimits::HardNodes; ++id)
                nodes.push_back(Node(id, BehaviorTreeOperation::Inverter, {id + 1}));
            nodes.push_back(Node(BehaviorTreeExecutionLimits::HardNodes, BehaviorTreeOperation::Task));
            Fixture fixture(std::move(nodes));
            auto plan = fixture.Plan();
            CHECK(plan->Depth() == BehaviorTreeExecutionLimits::HardNodes);
            auto instance = fixture.Instance();
            CHECK(fixture.Evaluate(*instance, 1) == AiTaskState::Failed);
            CHECK(fixture.probe->starts[BehaviorTreeExecutionLimits::HardNodes] == 1);
            ExpectError(BehaviorTreeExecutionPlan::Compile(fixture.Bindings(), Id(1), fixture.nodes, {}, {.maximumDepth = 64}),
                        AIErrors::BehaviorTreeLimitExceeded);
        }

        TEST_CASE("Exact blackboard publication fences and incompatible schema reload preserve running work", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[1] = AiTaskState::Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            auto foreign =
                BlackboardInstance::Create({fixture.agent, fixture.schema->Identity(), fixture.schema->Version(), 2, 2}, fixture.schema);
            REQUIRE(foreign.HasValue());
            auto snapshot = foreign.Value()->Snapshot();
            REQUIRE(snapshot.HasValue());
            ExpectError(instance->Evaluate(2, snapshot.Value(), fixture.agent), AIErrors::BlackboardInstanceInvalid);
            Fixture replacement({Node(1, BehaviorTreeOperation::Task)});
            ExpectError(instance->Replace(replacement.Plan()), AIErrors::DecisionAssetSchemaIncompatible);
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.probe->aborts[1] == 0);
            CHECK(fixture.probe->starts[1] == 1);
        }

        TEST_CASE("Steady-state traversal restart cancellation and looping allocate no core storage", "[unit][ai][bt_runtime]") {
            auto loop = Node(2, BehaviorTreeOperation::Loop, {4});
            loop.iterations = 3;
            Fixture fixture({Node(1, BehaviorTreeOperation::Parallel, {2, 3}), loop, Node(3, BehaviorTreeOperation::Task),
                             Node(4, BehaviorTreeOperation::Task)},
                            {{Id(5), Id(1), BehaviorTreeServiceMode::Reactive, 1}});
            fixture.probe->outcome[3] = AiTaskState::Running;
            auto instance = fixture.Instance();
            auto snapshot = fixture.blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            const auto before = allocations;
            auto first = instance->Evaluate(1, snapshot.Value(), fixture.agent);
            auto second = instance->Evaluate(2, snapshot.Value(), fixture.agent);
            auto third = instance->Evaluate(3, snapshot.Value(), fixture.agent);
            auto restarted = instance->Restart();
            auto fourth = instance->Evaluate(4, snapshot.Value(), fixture.agent);
            instance->Shutdown();
            const auto after = allocations;
            CHECK(after == before);
            CHECK(first.HasValue());
            CHECK(second.HasValue());
            CHECK(third.HasValue());
            CHECK(fourth.HasValue());
            CHECK(restarted.HasValue());
        }

        TEST_CASE("Replacement storage failure rolls back without aborting the active generation", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2}), Node(2, BehaviorTreeOperation::Task)});
            fixture.probe->outcome[2] = AiTaskState::Running;
            auto instance = fixture.Instance();
            REQUIRE(fixture.Evaluate(*instance, 1) == AiTaskState::Running);
            const auto handle = fixture.probe->handles[2];
            fixture.nodes[0].operation = BehaviorTreeOperation::Selector;
            auto candidate = fixture.Plan();
            failNextAllocation = true;
            auto replaced = instance->Replace(candidate);
            failNextAllocation = false;
            ExpectError(replaced, AIErrors::BehaviorTreeStorageUnavailable);
            CHECK(fixture.probe->aborts[2] == 0);
            CHECK(fixture.Evaluate(*instance, 2) == AiTaskState::Running);
            CHECK(fixture.probe->handles[2] == handle);
        }

        TEST_CASE("Detached cyclic controls invalid attachments and illegal priority observers never publish a plan",
                  "[unit][ai][bt_runtime]") {
            Fixture cyclic({Node(1, BehaviorTreeOperation::Task), Node(2, BehaviorTreeOperation::Sequence, {3}),
                            Node(3, BehaviorTreeOperation::Sequence, {2})});
            ExpectError(BehaviorTreeExecutionPlan::Compile(cyclic.Bindings(), Id(1), cyclic.nodes), AIErrors::BehaviorTreeCycle);
            auto check = Node(1, BehaviorTreeOperation::BlackboardCheck, {2});
            check.abort = BehaviorTreeAbortMode::Both;
            Fixture illegal({check, Node(2, BehaviorTreeOperation::Task)});
            ExpectError(BehaviorTreeExecutionPlan::Compile(illegal.Bindings(), Id(1), illegal.nodes),
                        AIErrors::BehaviorTreeTopologyInvalid);
            Fixture invalidService({Node(1, BehaviorTreeOperation::Task)}, {{Id(2), Id(99), BehaviorTreeServiceMode::Reactive, 1}});
            auto bindings = invalidService.Bindings();
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalidService.nodes, invalidService.services),
                        AIErrors::BehaviorTreeTopologyInvalid);
            invalidService.services[0].intervalTicks = 0;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalidService.nodes, invalidService.services),
                        AIErrors::BehaviorTreeSchemaInvalid);
        }

        TEST_CASE("Execution compiler rejects malformed enums cardinality identities topology and limits", "[unit][ai][bt_runtime]") {
            Fixture fixture({Node(1, BehaviorTreeOperation::Sequence, {2}), Node(2, BehaviorTreeOperation::Task)});
            auto bindings = fixture.Bindings();
            auto invalid = fixture.nodes;
            invalid[0].operation = BehaviorTreeOperation::Count;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            invalid = fixture.nodes;
            invalid[0].children = {Id(999)};
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeTopologyInvalid);
            invalid = fixture.nodes;
            invalid[0].children = {Id(2), Id(2)};
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeTopologyInvalid);
            invalid = fixture.nodes;
            invalid[0].children.clear();
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            invalid = fixture.nodes;
            invalid[1].id = Id(1);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeIdentityConflict);
            invalid = fixture.nodes;
            invalid[1].abort = BehaviorTreeAbortMode::Self;
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), invalid), AIErrors::BehaviorTreeSchemaInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(999), fixture.nodes), AIErrors::BehaviorTreeTopologyInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(fixture.Bindings(DecisionPlanKind::StateMachine), Id(1), fixture.nodes),
                        AIErrors::BehaviorTreeSchemaInvalid);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), fixture.nodes, {}, {.maximumNodes = 1}),
                        AIErrors::BehaviorTreeLimitExceeded);
            ExpectError(BehaviorTreeExecutionPlan::Compile(bindings, Id(1), fixture.nodes, {}, {.maximumDepth = 1025}),
                        AIErrors::BehaviorTreeLimitExceeded);
        }
    }  // namespace
}  // namespace Horo::AI
