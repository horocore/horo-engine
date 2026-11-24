#pragma once

#include "Horo/AI/DecisionGraphCompiler.h"
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "editor/screens/workspace/ai/decisions/panels/DecisionStateMachineAuthoring.h"

namespace Horo::Editor {
    /** @brief Complete saved subtree inputs pinned until canonical validation has finished. */
    struct DecisionTreeClosure final {
        std::vector<AI::DecisionGraphSource> graphs;
        std::optional<Assets::AssetCookInputSnapshot> inputs;
    };

    /** @brief True when the authored Behavior Tree contains any persisted call. */
    [[nodiscard]] bool HasDecisionTreeCalls(const AI::DecisionGraphSource &source);
    /** @brief Resolves the bounded reachable saved BT closure by canonical graph identity. */
    [[nodiscard]] Result<DecisionTreeClosure> CaptureDecisionTreeClosure(const std::filesystem::path &root,
                                                                         const Assets::AssetRegistrySnapshot &registry,
                                                                         const AI::DecisionGraphSource &candidate,
                                                                         const std::optional<BehaviorTreeSubtreeReview> &review);
}  // namespace Horo::Editor
