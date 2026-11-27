#pragma once

#include "Horo/AI/DecisionGraphCompiler.h"
#include "editor/screens/workspace/ai/decisions/panels/DecisionStateMachineAuthoring.h"

namespace Horo::Editor {
    /** @brief Checks whether root bindings belong to the supported score-only policy. */
    [[nodiscard]] bool CanAuthorUtilityGraph(const AI::UtilityDecisionSource &source);
    /** @brief Applies one complete typed candidate before workspace compiler and history admission. */
    [[nodiscard]] Result<bool> EditUtilityGraphCandidate(AI::DecisionGraphSource &source, const UtilityGraphEditRequest &request,
                                                         const AI::DecisionNodeContract *admittedTask);
}  // namespace Horo::Editor
