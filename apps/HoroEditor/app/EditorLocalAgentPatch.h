#pragma once

/** @file EditorLocalAgentPatch.h @brief Read-only review input from one exact approved source turn. */
#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor {
    /** @brief Immutable evidence retained from a full source attachment to a local turn. */
    struct EditorLocalAgentPatchSource final {
        std::vector<SourceDocumentSnapshot> sources;
        std::string model;
        std::uint64_t turn{};
    };

    /** @brief File operation shown in the chat review summary. */
    enum class EditorPatchFileKind : std::uint8_t {
        Create,
        Edit,
        Delete,
        Rename
    };

    /** @brief Counts describe original and replacement bytes in the proposed ranges. */
    struct EditorPatchFileCard final {
        SourceDocumentId path;
        std::optional<SourceDocumentId> destination;
        EditorPatchFileKind kind{};
        std::size_t hunks{};
        std::uint64_t removedBytes{};
        std::uint64_t addedBytes{};
    };

    /** @brief Projects file operations in canonical review order, retaining paths and change counts only. */
    [[nodiscard]] std::vector<EditorPatchFileCard> DescribeEditorPatchFiles(const Proposals::ChangeSet &proposal);

    /** @brief An admitted source edit and its read-only preview, ready for an explicit workspace review action. */
    struct EditorLocalAgentPatchReview final {
        Proposals::ChangeSet proposal;
        Proposals::WorkspaceEditPlan plan;
        std::vector<EditorPatchFileCard> files;
        Proposals::ReviewDecisions decisions;
        std::optional<std::size_t> expandedFile;
        std::size_t displayedHunk{};
    };

    /** @brief Reports whether a complete reply begins with a supported patch format. */
    [[nodiscard]] bool IsEditorLocalPatchReply(std::string_view answer);

    /** @brief Prepares a complete supported reply using just the exact source and project offered to its turn. */
    [[nodiscard]] Result<EditorLocalAgentPatchReview> PrepareEditorLocalPatchReview(std::string_view answer,
                                                                                    const EditorLocalAgentPatchSource &source,
                                                                                    std::string_view project, std::uint64_t generation);
}  // namespace Horo::Editor
