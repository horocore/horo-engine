#pragma once
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI::Detail {
    struct DecisionClosureLayout final {
        std::vector<DecisionCallOccurrence> occurrences;
        DecisionStorageRequirements storage;
    };

    [[nodiscard]] Result<DecisionClosureLayout> BuildDecisionClosureLayout(std::span<const DecisionCompiledGraph> graphs, std::size_t root,
                                                                           const DecisionCompilationLimits &limits);
}  // namespace Horo::AI::Detail
