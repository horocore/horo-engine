#pragma once

#include "Horo/AI/DecisionIdentity.h"

#include <array>
#include <cstdint>
#include <span>

namespace Horo::Editor {
    struct DecisionTaskChoice;
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;

    /** @brief Owns the search and selection for one Decision document's task picker. */
    struct DecisionTaskPaletteDraft final {
        std::array<char, 128> search{};
        AI::DecisionNodeTypeId selected;
        std::uint64_t catalogRevision{};
    };

    /** @brief Draws the themed search and picker for the current model's registered task choices.
     * @param choices Choices already admitted for the current Decision graph family.
     * @param catalogRevision Generation of the admitted task choices.
     * @param draft Document-owned transient search and selection.
     * @param context Editor theme and localization.
     * @param command Reports active text input to the workspace shortcut owner.
     * @return Borrowed current descriptor choice, or null when no visible choice is selected.
     */
    [[nodiscard]] const DecisionTaskChoice *DrawDecisionTaskPalette(std::span<const DecisionTaskChoice> choices,
                                                                    std::uint64_t catalogRevision, DecisionTaskPaletteDraft &draft,
                                                                    const EditorGuiContext &context,
                                                                    EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
