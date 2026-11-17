#pragma once
#include "DecisionCompileDetail.h"

namespace Horo::AI::Detail {
    struct DecisionCompilationCandidate final {
        std::vector<DecisionGraphSource> sources;
        std::vector<DecisionCompiledGraph> graphs;
        std::vector<DecisionAssetValidationDiagnostic> diagnostics;
        std::size_t root{};
    };

    [[nodiscard]] Result<DecisionCompilationCandidate> BuildDecisionCompilation(DecisionGraphAssetId root,
                                                                                std::span<const DecisionGraphSource> sources,
                                                                                const DecisionCompilationEnvironment &environment,
                                                                                const DecisionCompilationLimits &limits,
                                                                                DecisionClosureAdmission admission);
}  // namespace Horo::AI::Detail
