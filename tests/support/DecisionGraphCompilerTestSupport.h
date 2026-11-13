#pragma once
#include "../unit/runtime/ai/AiTestSupport.h"
#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionPlanCodec.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <functional>
#include <string>

namespace Horo::AI::DecisionGraphTestSupport {
    using TestSupport::MakeIdentity;

    inline SourceLocation Location() {
        return {"tests/support/DecisionGraphCompilerTestSupport.h", 0, 0};
    }

    inline DecisionNodeId Node(std::uint64_t value) {
        return MakeIdentity<DecisionNodeId>(value);
    }

    inline DecisionGraphAssetId Graph(std::uint64_t value) {
        return MakeIdentity<DecisionGraphAssetId>(value);
    }

    inline BlackboardSchemaId SchemaId() {
        return MakeIdentity<BlackboardSchemaId>(100);
    }

    inline DecisionNodeTypeId Type(std::uint64_t value) {
        return MakeIdentity<DecisionNodeTypeId>(value);
    }

    struct Probe final {
        std::vector<std::string> calls;
        std::vector<TaskHandle> tasks;
        std::vector<TaskHandle> abortedTasks;
        std::uint64_t succeedTick{2};
        std::size_t factories{};
        std::size_t starts{};
        std::size_t resumes{};
        std::size_t aborts{};
        std::size_t cleanups{};
        std::function<void()> onStart;
    };

    class Provider final : public IBehaviorTreeExecutor {
    public:
        explicit Provider(Probe &probe, bool immediate = false) : probe_(probe), immediate_(immediate) {}

        Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &operation) noexcept override {
            ++probe_.starts;
            probe_.calls.push_back("start:" + std::to_string(operation.taskDefinition.Value()));
            probe_.tasks.push_back(operation.task);
            if (probe_.onStart)
                probe_.onStart();
            return Result<AiTaskState>::Success((immediate_ || context.tick >= probe_.succeedTick) ? AiTaskState::Succeeded
                                                                                                   : AiTaskState::Running);
        }

        Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context, const AiTaskOperationContext &) noexcept override {
            ++probe_.resumes;
            return Result<AiTaskState>::Success((immediate_ || context.tick >= probe_.succeedTick) ? AiTaskState::Succeeded
                                                                                                   : AiTaskState::Running);
        }

        void Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason) noexcept override {
            ++probe_.aborts;
            probe_.abortedTasks.push_back(operation.task);
            probe_.calls.push_back("abort");
        }

        void Cleanup(const AiTaskOperationContext &, const AiTaskTerminalResult &) noexcept override {
            ++probe_.cleanups;
            probe_.calls.push_back("cleanup");
        }

        Result<bool> Check(const BehaviorTreeEvaluationContext &) noexcept override {
            return Result<bool>::Success(true);
        }

        Result<void> Service(const BehaviorTreeEvaluationContext &) noexcept override {
            return Result<void>::Success();
        }

    private:
        Probe &probe_;
        bool immediate_{};
    };

    inline IBehaviorTreeExecutor *Create(void *owner, std::span<const BehaviorTreeProperty>) {
        auto &probe = *static_cast<Probe *>(owner);
        ++probe.factories;
        return new Provider(probe);
    }

    inline IBehaviorTreeExecutor *CreateImmediate(void *owner, std::span<const BehaviorTreeProperty>) {
        auto &probe = *static_cast<Probe *>(owner);
        ++probe.factories;
        return new Provider(probe, true);
    }

    inline void Destroy(void *, IBehaviorTreeExecutor *provider) noexcept {
        delete provider;
    }

    inline DecisionNodeRegistration Registration(Probe &probe, std::uint64_t type = 501, bool immediate = false) {
        DecisionNodeContract contract;
        contract.descriptor = {Type(type), {DecisionDescriptorSourceKind::Native, MakeIdentity<DecisionProviderId>(50), 1}, {}, Location()};
        contract.role = DecisionNodeRole::Task;
        contract.memory = {DecisionProviderMemoryKind::NativeDeclared, sizeof(Provider), 0, 0};
        return {std::move(contract), {&probe, immediate ? CreateImmediate : Create, Destroy}};
    }

    inline BehaviorTreeNode TreeNode(std::uint64_t id, DecisionControlKind kind) {
        BehaviorTreeNode node;
        node.id = MakeIdentity<BehaviorTreeNodeId>(id);
        node.displayName = "fixture";
        node.control.kind = kind;
        node.source = Location();
        node.pins = {{MakeIdentity<BehaviorTreePinId>(id * 2), BehaviorTreePinDirection::Input},
                     {MakeIdentity<BehaviorTreePinId>(id * 2 + 1), BehaviorTreePinDirection::Output}};
        if (kind == DecisionControlKind::Root) {
            node.kind = BehaviorTreeNodeKind::Root;
            node.minimumChildren = 1;
            node.maximumChildren = 1;
        } else if (kind == DecisionControlKind::Parallel) {
            node.kind = BehaviorTreeNodeKind::Composite;
            node.minimumChildren = 2;
            node.maximumChildren = 2;
        } else {
            node.kind = BehaviorTreeNodeKind::Task;
            if (kind == DecisionControlKind::Task) {
                node.type = MakeIdentity<BehaviorTreeNodeTypeId>(501);
                node.task = MakeIdentity<TaskId>(5000 + id);
            }
        }
        return node;
    }

    inline BehaviorTreeEdge Edge(std::uint64_t id, std::uint64_t parent, std::uint64_t child, std::size_t order = 0) {
        return {MakeIdentity<BehaviorTreeEdgeId>(id),
                BehaviorTreeEdgeKind::Child,
                MakeIdentity<BehaviorTreeNodeId>(parent),
                MakeIdentity<BehaviorTreePinId>(parent * 2 + 1),
                MakeIdentity<BehaviorTreeNodeId>(child),
                MakeIdentity<BehaviorTreePinId>(child * 2),
                order};
    }

    inline DecisionGraphSource Tree(std::uint64_t graph, std::vector<BehaviorTreeNode> nodes, std::vector<BehaviorTreeEdge> edges) {
        auto captured = BehaviorTreeAsset::Capture({.graph = MakeIdentity<BehaviorTreeGraphId>(graph),
                                                    .displayName = "fixture",
                                                    .root = MakeIdentity<BehaviorTreeNodeId>(1),
                                                    .nodes = std::move(nodes),
                                                    .edges = std::move(edges)});
        REQUIRE(captured.HasValue());
        return {Graph(graph),
                1,
                SchemaId(),
                {1, 1},
                Location(),
                {},
                std::make_shared<const BehaviorTreeAsset>(std::move(captured).Value())};
    }

    inline DecisionGraphSource TaskTree(std::uint64_t graph, std::uint64_t type = 501) {
        auto task = TreeNode(2, DecisionControlKind::Task);
        task.type = MakeIdentity<BehaviorTreeNodeTypeId>(type);
        return Tree(graph, {TreeNode(1, DecisionControlKind::Root), std::move(task)}, {Edge(1, 1, 2)});
    }

    inline DecisionGraphSource CallTree(std::uint64_t graph, std::uint64_t child, DecisionPlanKind family) {
        auto call = TreeNode(2, DecisionControlKind::Subplan);
        call.call = DecisionSubplanCall{Graph(child), family, {1, 1}};
        return Tree(graph, {TreeNode(1, DecisionControlKind::Root), std::move(call)}, {Edge(1, 1, 2)});
    }

    inline DecisionGraphSource ParallelCalls(std::uint64_t graph, std::uint64_t child, DecisionPlanKind family) {
        auto first = TreeNode(3, DecisionControlKind::Subplan), second = TreeNode(4, DecisionControlKind::Subplan);
        first.call = DecisionSubplanCall{Graph(child), family, {1, 1}};
        second.call = first.call;
        return Tree(graph, {TreeNode(1, DecisionControlKind::Root), TreeNode(2, DecisionControlKind::Parallel), first, second},
                    {Edge(1, 1, 2), Edge(2, 2, 3), Edge(3, 2, 4, 1)});
    }

    inline DecisionGraphSource TerminalMachine(std::uint64_t graph, DecisionStateTerminal terminal = DecisionStateTerminal::Succeeded) {
        StateMachineAssetDescriptor machine{.initial = Node(1), .states = {{.id = Node(1), .terminal = terminal, .source = Location()}}};
        return {Graph(graph), 1, SchemaId(), {1, 1}, Location(), MakeIdentity<TaskId>(10000 + graph), std::move(machine)};
    }

    inline DecisionGraphSource CallingMachine(std::uint64_t graph, std::uint64_t child, DecisionPlanKind family) {
        StateMachineState calling{.id = Node(1), .call = DecisionSubplanCall{Graph(child), family, {1, 1}}, .source = Location()};
        StateMachineState done{.id = Node(2), .terminal = DecisionStateTerminal::Succeeded, .source = Location()};
        StateMachineTransition finished{.id = Node(3),
                                        .source = Node(1),
                                        .target = Node(2),
                                        .childOutcome = AiTaskState::Succeeded,
                                        .location = Location()};
        StateMachineAssetDescriptor machine{.initial = Node(1), .states = {calling, done}, .transitions = {finished}};
        return {Graph(graph), 1, SchemaId(), {1, 1}, Location(), MakeIdentity<TaskId>(10000 + graph), std::move(machine)};
    }

    struct Fixture {
        struct ProviderOwner final {
            Probe probe;
            std::atomic_bool admission{true};
        };

        std::shared_ptr<ProviderOwner> providerOwner{std::make_shared<ProviderOwner>()};
        Probe &probe{providerOwner->probe};
        std::shared_ptr<std::atomic_bool> admission{providerOwner, &providerOwner->admission};
        std::shared_ptr<const DecisionNodeGeneration> providers;
        std::shared_ptr<const BlackboardSchema> schema;
        BlackboardInstanceBinding binding;
        std::unique_ptr<BlackboardInstance> blackboard;
        CancellationSource cancellation;

        Fixture() {
            const std::array keys{BlackboardKeyDescriptor{.key = MakeIdentity<BlackboardKeyId>(1),
                                                          .defaultValue = BlackboardValue{BlackboardScalarValue{true}}}};
            auto captured = BlackboardSchema::Capture({SchemaId(), 1, BlackboardUnknownValuePolicy::Reject, keys});
            REQUIRE(captured.HasValue());
            schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
            binding = {{AiRuntimeIncarnation::Create(77).Value(), {3, 4}}, schema->Identity(), 1, 1, 1};
            auto created = BlackboardInstance::Create(binding, schema);
            REQUIRE(created.HasValue());
            blackboard = std::move(created).Value();
            auto registration = Registration(probe);
            auto generation = DecisionNodeGeneration::Capture({&registration, 1}, admission);
            REQUIRE(generation.HasValue());
            providers = std::move(generation).Value();
        }

        DecisionCompilationEnvironment Environment() const {
            return {providers, {schema}, {}, 1};
        }

        std::shared_ptr<const CookedDecisionPlan> Compile(std::span<const DecisionGraphSource> sources, std::uint64_t root) {
            auto result = DecisionGraphCompiler::Compile(Graph(root), sources, Environment());
            REQUIRE(result.HasValue());
            std::string diagnostics;
            for (const auto &diagnostic : result.Value().validation.Diagnostics())
                diagnostics += diagnostic.message + "\n";
            INFO(diagnostics);
            REQUIRE(result.Value().plan);
            return result.Value().plan;
        }

        DecisionGraphInstanceBinding RuntimeBinding() const {
            return {binding,
                    {MakeIdentity<TaskId>(9000), {binding.agent.incarnation, {99, 1}}, binding.agent, cancellation.Token()},
                    100,
                    1U << 20U,
                    1};
        }

        std::unique_ptr<DecisionGraphInstance> Instance(std::shared_ptr<const CookedDecisionPlan> plan) {
            auto result = DecisionGraphInstance::Create(std::move(plan), providers, RuntimeBinding());
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        AiTaskState Step(DecisionGraphInstance &instance, std::uint64_t tick) {
            auto snapshot = blackboard->Snapshot();
            REQUIRE(snapshot.HasValue());
            auto result = instance.Step({tick, binding, snapshot.Value(), {}});
            REQUIRE(result.HasValue());
            return result.Value();
        }
    };
}  // namespace Horo::AI::DecisionGraphTestSupport
