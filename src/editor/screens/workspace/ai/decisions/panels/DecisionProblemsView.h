#pragma once

#include "Horo/AI/DecisionIdentity.h"

#include <span>

namespace Horo::AI {
    struct DecisionAssetValidationDiagnostic;
}

namespace Horo::Editor {
    struct DecisionGraphWorkspaceDocument;
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;

    /** @brief Draws the bounded Decision Problems list and emits navigation for the activated finding. */
    [[nodiscard]] AI::DecisionNodeId DrawDecisionProblems(const DecisionGraphWorkspaceDocument &document,
                                                          std::span<const AI::DecisionAssetValidationDiagnostic> findings,
                                                          const EditorGuiContext &context, EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
