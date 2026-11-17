#pragma once
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI::Detail {
    struct LoweredDecisionGraph final {
        DecisionAssetDescriptor metadata;
        DecisionNodeId root;
        std::vector<BehaviorTreeExecutionNode> controls;
        std::vector<BehaviorTreeExecutionService> services;
        std::optional<StateMachineAssetDescriptor> stateMachine;
        std::optional<UtilityDecisionSource> utility;
        std::vector<DecisionNodeConfiguration> properties;
        std::vector<std::pair<DecisionNodeId, DecisionSubplanCall>> calls;
    };

    /** @brief Checks bounded Utility source containers before copying or encoding them. */
    [[nodiscard]] bool ValidUtilitySourceBounds(const UtilityDecisionSource &source) noexcept;
    [[nodiscard]] Result<void> LowerUtility(LoweredDecisionGraph &graph, const UtilityDecisionSource &source,
                                            const SourceLocation &location);

    [[nodiscard]] Result<LoweredDecisionGraph> LowerDecisionGraph(const DecisionGraphSource &source);
    [[nodiscard]] Result<void> ValidateDecisionProviders(const LoweredDecisionGraph &source,
                                                         const DecisionCompilationEnvironment &environment);
    [[nodiscard]] Result<DecisionStorageRequirements> MeasureDecisionGraph(const DecisionCompiledGraph &graph,
                                                                           const DecisionCompilationEnvironment &environment);
    [[nodiscard]] Result<Sha256Digest> DecisionEnvironmentIdentity(const DecisionCompilationEnvironment &environment);
}  // namespace Horo::AI::Detail
