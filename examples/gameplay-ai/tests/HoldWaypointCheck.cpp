#include "Horo/Gameplay/GameModule.h"
#include "Horo/Gameplay/NativeDecisionNode.h"

#include <array>
#include <iostream>

extern "C" const Horo::Gameplay::GeneratedGameplayDescriptorBundle *GetGameplayDescriptorBundle() noexcept;

namespace {
    using namespace Horo;
    using namespace Horo::AI;

    /** @brief Checks the published native task with the same frozen input at three fixed ticks. */
    bool CheckHold(const DecisionNodeRegistration &registration, const BlackboardSnapshot &snapshot) {
        const auto &factory = registration.factory;
        const auto destroy = [&](IBehaviorTreeExecutor *instance) {
            factory.destroy(factory.userData, instance);
        };
        const std::unique_ptr<IBehaviorTreeExecutor, decltype(destroy)> node{factory.create(factory.userData,
                                                                                            registration.contract.properties),
                                                                             destroy};
        const auto id = DecisionNodeId::Create(1).Value();
        const AiTaskOperationContext operation;
        const auto start = node->Start({id, {}, snapshot, 10}, operation);
        const auto waiting = node->Resume({id, {}, snapshot, 39}, operation);
        const auto reached = node->Resume({id, {}, snapshot, 40}, operation);
        return start.HasValue() && start.Value() == AiTaskState::Running && waiting.HasValue() && waiting.Value() == AiTaskState::Running &&
               reached.HasValue() && reached.Value() == AiTaskState::Succeeded;
    }
}  // namespace

int main() {
    const auto *bundle = GetGameplayDescriptorBundle();
    if (!bundle || bundle->diagnosticCount != 0 || bundle->decisionNodeCount != 1 ||
        bundle->decisionNodes[0].contract.descriptor.type.Value() != 83002) {
        std::cerr << "Expected the generated Hold Waypoint decision descriptor.\n";
        return 1;
    }

    const auto schemaId = BlackboardSchemaId::Create(83003).Value();
    const std::array keys{
        BlackboardKeyDescriptor{.key = BlackboardKeyId::Create(1).Value(), .defaultValue = BlackboardValue{BlackboardScalarValue{false}}}};
    auto captured = BlackboardSchema::Capture({.identity = schemaId, .version = 1, .keys = keys});
    if (captured.HasError()) {
        std::cerr << captured.ErrorValue().message << '\n';
        return 1;
    }
    const auto schema = std::make_shared<const BlackboardSchema>(std::move(captured).Value());
    const AgentHandle agent{AiRuntimeIncarnation::Create(1).Value(), {0, 1}};
    const auto board = BlackboardInstance::Create({agent, schemaId, schema->Version(), 1, 1}, schema);
    if (board.HasError()) {
        std::cerr << board.ErrorValue().message << '\n';
        return 1;
    }
    const auto snapshot = board.Value()->Snapshot();
    if (snapshot.HasError() || !CheckHold(bundle->decisionNodes[0], snapshot.Value())) {
        std::cerr << "The generated task did not wait for exactly 30 fixed ticks.\n";
        return 1;
    }
    return 0;
}
