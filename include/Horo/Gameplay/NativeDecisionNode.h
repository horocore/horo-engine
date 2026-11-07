#pragma once
/** @file NativeDecisionNode.h
 * @brief Native decision-node annotations discovered by the existing gameplay descriptor generator.
 */
#include "Horo/AI/DecisionNodeRegistration.h"

#include <type_traits>

namespace Horo::Gameplay {
    /** @brief Parses the annotation's exact non-zero decimal persistent identity. */
    [[nodiscard]] AI::DecisionNodeTypeId ParseNativeDecisionNodeIdentity(std::string_view identity);

    template <typename Node>
    [[nodiscard]] AI::DecisionNodeRegistration MakeNativeDecisionNodeRegistration(std::string_view identity, SourceLocation source) {
        static_assert(std::is_base_of_v<AI::IBehaviorTreeExecutor, Node>);
        AI::DecisionNodeRegistration registration;
        registration.contract = Node::DescribeDecisionNode();
        registration.contract.descriptor.type = ParseNativeDecisionNodeIdentity(identity);
        if (registration.contract.descriptor.source.source.empty())
            registration.contract.descriptor.source = source;
        for (auto &requirement : registration.contract.descriptor.requirements)
            if (requirement.source.source.empty())
                requirement.source = source;
        registration.factory.create = [](void *, std::span<const AI::BehaviorTreeProperty> properties) -> AI::IBehaviorTreeExecutor * {
            return std::make_unique<Node>(properties).release();
        };
        registration.factory.destroy = [](void *, AI::IBehaviorTreeExecutor *instance) noexcept {
            delete static_cast<Node *>(instance);
        };
        return registration;
    }
}  // namespace Horo::Gameplay

/** @brief Marks a module-owned node; StableTypeId is a quoted non-zero decimal uint64 identity. */
#define HORO_AI_NODE(Type, StableTypeId)                                                                                                   \
    Horo::AI::DecisionNodeRegistration HoroGeneratedDecisionNode_##Type() {                                                                \
        return Horo::Gameplay::MakeNativeDecisionNodeRegistration<Type>(StableTypeId, {__FILE__, __LINE__, 1});                            \
    }
