#pragma once

#include "Horo/AI/DecisionIdentity.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace Horo::AI {
    enum class AiCapability : std::uint8_t;
    enum class DecisionNodeRole : std::uint8_t;
    class BlackboardSchema;
    class DecisionBlackboardProviderMetadata;
    struct DecisionGraphSource;
}  // namespace Horo::AI

namespace Horo::Editor {
    struct BlackboardSchemaDocumentSnapshot;
    struct DecisionTaskChoice;
    struct EditorGuiContext;
    struct EditorWorkspaceViewCommandData;
    struct EditorWorkspaceViewModel;

    /** @brief Finds the uniquely open, clean schema admitted by the Decision graph.
     * @param model Current workspace document snapshots.
     * @param source Graph declaring the schema identity and version range.
     * @return Borrowed saved schema snapshot or null for absent, dirty, conflicting or ambiguous evidence.
     */
    [[nodiscard]] const BlackboardSchemaDocumentSnapshot *FindSavedDecisionSchema(const EditorWorkspaceViewModel &model,
                                                                                  const AI::DecisionGraphSource &source);

    /** @brief Exact current family choices and optional read-only compatibility evidence. */
    struct DecisionTaskPaletteCatalog final {
        std::span<const DecisionTaskChoice> choices;
        std::uint64_t revision{};
        const AI::DecisionBlackboardProviderMetadata *providers{};
        const AI::BlackboardSchema *schema{};
        bool allowQueryOutput{true};
    };

    /** @brief Owns the search and selection for one Decision document's task picker. */
    struct DecisionTaskPaletteDraft final {
        std::array<char, 128> search{};
        AI::DecisionNodeTypeId selected;
        std::uint64_t catalogRevision{};
        std::optional<AI::AiCapability> capability;
        std::optional<AI::DecisionDescriptorSourceKind> origin;
        std::optional<AI::DecisionNodeRole> role;
    };

    /** @brief Draws the themed search and picker for the current model's registered task choices.
     * @param catalog Family-admitted choices and their optional saved schema and provider evidence.
     * @param draft Document-owned transient search and selection.
     * @param context Editor theme and localization.
     * @param command Reports active text input to the workspace shortcut owner.
     * @return Borrowed current descriptor choice, or null when no visible choice is selected.
     */
    [[nodiscard]] const DecisionTaskChoice *DrawDecisionTaskPalette(const DecisionTaskPaletteCatalog &catalog,
                                                                    DecisionTaskPaletteDraft &draft, const EditorGuiContext &context,
                                                                    EditorWorkspaceViewCommandData &command);
}  // namespace Horo::Editor
