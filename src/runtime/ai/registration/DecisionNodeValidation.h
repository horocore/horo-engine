#pragma once
#include "Horo/AI/DecisionNodeRegistration.h"

namespace Horo::AI::Detail {
    [[nodiscard]] bool ValidDecisionProperty(const BehaviorTreeProperty &property) noexcept;
    [[nodiscard]] Result<std::vector<BehaviorTreeProperty>> BindDecisionProperties(const DecisionNodeContract &contract,
                                                                                   std::span<const BehaviorTreeProperty> authored);
}  // namespace Horo::AI::Detail
