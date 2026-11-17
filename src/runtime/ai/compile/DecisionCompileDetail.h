#pragma once
#include "Horo/AI/DecisionGraphCompiler.h"

namespace Horo::AI::Detail {
    struct LoweredDecisionGraph final {
        DecisionAssetDescriptor metadata;
        DecisionNodeId root;
        std::vector<BehaviorTreeExecutionNode> controls;
        std::vector<BehaviorTreeExecutionService> services;
        std::optional<StateMachineAssetDescriptor> stateMachine;
        std::vector<DecisionNodeConfiguration> properties;
        std::vector<std::pair<DecisionNodeId, DecisionSubplanCall>> calls;
    };

    [[nodiscard]] Result<LoweredDecisionGraph> LowerDecisionGraph(const DecisionGraphSource &source);
    [[nodiscard]] Result<void> ValidateDecisionProviders(const LoweredDecisionGraph &source,
                                                         const DecisionCompilationEnvironment &environment);
    [[nodiscard]] Result<DecisionStorageRequirements> MeasureDecisionGraph(const DecisionCompiledGraph &graph,
                                                                           const DecisionCompilationEnvironment &environment);
    [[nodiscard]] Result<Sha256Digest> DecisionEnvironmentIdentity(const DecisionCompilationEnvironment &environment);
}  // namespace Horo::AI::Detail
