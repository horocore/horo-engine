#pragma once

#include "AiTestSupport.h"
#include "Horo/AI/BehaviorTreeRuntime.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <utility>

namespace Horo::AI::BehaviorTreeTestSupport {
    template <typename T>
    concept AllowsUnadmittedConstruction = requires { T({}); };

    static_assert(!AllowsUnadmittedConstruction<BehaviorTreeExecutionPlan>);
    static_assert(!AllowsUnadmittedConstruction<BehaviorTreeInstance>);

    using TestSupport::ExpectError;
    using TestSupport::MakeIdentity;

    inline DecisionNodeId Id(const std::uint64_t value) {
        return MakeIdentity<DecisionNodeId>(value);
    }

    inline SourceLocation Source() {
        return {"tests/ai/tree.horo_bt", 1, 1};
    }

    inline BehaviorTreeExecutionNode Node(const std::uint64_t id, const BehaviorTreeOperation operation,
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
        std::array<std::size_t, Capacity> starts{};
        std::array<std::size_t, Capacity> resumes{};
        std::array<std::size_t, Capacity> aborts{};
        std::array<std::size_t, Capacity> cleanups{};
        std::array<std::size_t, Capacity> checks{};
        std::array<std::size_t, Capacity> services{};
        std::array<TaskHandle, Capacity> handles{};
        std::array<std::optional<AiTaskTerminalResult>, Capacity> terminals{};
        std::array<std::uint64_t, Capacity * 4> order{};
        std::size_t orderCount{};
        std::size_t destroyed{};
        std::uint64_t failTask{};
        std::uint64_t failService{};
        std::uint64_t failCheck{};
        bool invalidOutput{};

        Probe() {
            outcome.fill(AiTaskState::Succeeded);
        }
    };

    class Executor final : public IBehaviorTreeExecutor {
    public:
        explicit Executor(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {}

        Executor(const Executor &) = delete;
        Executor &operator=(const Executor &) = delete;
        Executor(Executor &&) = delete;
        Executor &operator=(Executor &&) = delete;

        ~Executor() override {
            ++probe_->destroyed;
        }

        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &operation) noexcept override {
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
            const auto &stored = value.Value();
            const auto *scalar = stored ? std::get_if<BlackboardScalarValue>(&*stored) : nullptr;
            const auto *condition = scalar ? std::get_if<bool>(scalar) : nullptr;
            if (!condition)
                return Result<bool>::Failure(MakeError(AIErrors::BlackboardValueInvalid));
            return Result<bool>::Success(*condition);
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
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(10),
                                                          .kind = BlackboardValueKind::Boolean,
                                                          .cardinality = BlackboardValueCardinality::Scalar,
                                                          .maximumCollectionElements = 1,
                                                          .presence = BlackboardKeyPresence::Required,
                                                          .access = BlackboardKeyAccess::ReadWrite,
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{true}}}};
            auto captured = BlackboardSchema::Capture({.identity = MakeIdentity<BlackboardSchemaId>(7), .keys = keys});
            REQUIRE(captured.HasValue());
            schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
            auto storage = BlackboardInstance::Create({agent, schema->Identity(), schema->Version(), 1, 1}, schema);
            REQUIRE(storage.HasValue());
            blackboard = std::move(storage).Value();
        }

        std::shared_ptr<const DecisionAssetPlan> Bindings(const DecisionPlanKind kind = DecisionPlanKind::BehaviorTree) const {
            DecisionAssetDescriptor asset{.asset = MakeIdentity<DecisionGraphAssetId>(100),
                                          .kind = kind,
                                          .blackboardSchema = schema->Identity(),
                                          .source = Source()};
            for (const auto &node : nodes)
                asset.nodes.push_back({.id = node.id, .type = MakeIdentity<DecisionNodeTypeId>(101), .source = Source()});
            for (const auto &service : services)
                asset.nodes.push_back({.id = service.id, .type = MakeIdentity<DecisionNodeTypeId>(101), .source = Source()});
            const std::array descriptors{
                DecisionNodeDescriptor{.type = MakeIdentity<DecisionNodeTypeId>(101),
                                       .origin = {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(501), 1},
                                       .requirements = {{.key = MakeIdentity<BlackboardKeyId>(10),
                                                         .kind = BlackboardValueKind::Boolean,
                                                         .source = Source()}},
                                       .source = Source()}};
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
            auto result = blackboard->BeginWriteBatch();
            REQUIRE(result.HasValue());
            auto batch = std::move(result).Value();
            REQUIRE(batch.Stage({MakeIdentity<BlackboardKeyId>(10), BlackboardValue{BlackboardScalarValue{value}}}).HasValue());
            const auto committed = blackboard->CommitAtBlackboardSync(std::move(batch));
            REQUIRE(committed.HasValue());
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

}  // namespace Horo::AI::BehaviorTreeTestSupport
