#pragma once

/** @file EditorLocalAgentPatch.h @brief Read-only review input from one exact approved source turn. */
#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor {
    /** @brief Immutable evidence retained from a full source attachment to a local turn. */
    struct EditorLocalAgentPatchSource final {
        SourceDocumentSnapshot source;
        std::string model;
        std::uint64_t turn{};
    };

    /** @brief An admitted source edit and its read-only preview, ready for an explicit workspace review action. */
    struct EditorLocalAgentPatchReview final {
        Proposals::ChangeSet proposal;
        Proposals::WorkspaceEditPlan plan;
    };

    /** @brief Reports whether a complete reply begins with a supported patch format. */
    [[nodiscard]] bool IsEditorLocalPatchReply(std::string_view answer);

    /** @brief Prepares a complete supported reply using just the exact source and project offered to its turn. */
    [[nodiscard]] Result<EditorLocalAgentPatchReview> PrepareEditorLocalPatchReview(std::string_view answer,
                                                                                    const EditorLocalAgentPatchSource &source,
                                                                                    std::string_view project, std::uint64_t generation);
}  // namespace Horo::Editor
