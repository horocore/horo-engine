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

    /** @brief Observes approved create and rename destinations on an I/O worker.
     * @param cards Paths from an already admitted proposal. @param root Active project root.
     * @param files Host read authority. @param observations Existing exact open-source evidence.
     * @param stop Cooperative cancellation.
     */
    void ObserveEditorPatchDestinations(std::span<const EditorPatchFileCard> cards, const std::filesystem::path &root,
                                        const DurableFileSystem &files, std::vector<Proposals::SourcePathObservation> &observations,
                                        const CancellationToken &stop);

    /** @brief One destination explicitly selected for the current reply's read-only review. */
    struct EditorPatchDestination final {
        SourceDocumentId path;
        const std::filesystem::path &root;
        const DurableFileSystem &files;
        CancellationToken stop;
    };

    /** @brief An admitted source edit and its read-only preview, ready for an explicit workspace review action. */
    struct EditorLocalAgentPatchReview final {
        Proposals::ChangeSet proposal;
        std::shared_ptr<const Proposals::WorkspaceEditPlan> plan;
        std::vector<EditorPatchFileCard> files;
        Proposals::ReviewDecisions decisions;
        std::vector<SourceDocumentId> offeredPaths;
        std::optional<std::size_t> expandedFile;
        std::size_t displayedHunk{};
    };

    /** @brief Gives the available apply route for the accepted selection, including single-file confirmation.
     * @return Edit for text, Create for drafts, Rename for a group containing renames, Delete for a single delete, or no route.
     */
    [[nodiscard]] std::optional<EditorPatchFileKind> EditorPatchApplyKind(const EditorLocalAgentPatchReview &review);

    /** @brief Reports whether a complete reply begins with a supported patch format. */
    [[nodiscard]] bool IsEditorLocalPatchReply(std::string_view answer);

    /** @brief Prepares a complete supported reply using the exact offered sources and optional read-only destination.
     * @param answer Complete model reply. @param source Exact sources originally offered to the turn.
     * @param project Current project identity. @param generation Current project attachment generation.
     * @param destination Optional exact destination selected by the user. When supplied this runs on the I/O worker.
     * @return Detached review or a typed admission failure. No source is opened, created, or changed.
     */
    [[nodiscard]] Result<EditorLocalAgentPatchReview> PrepareEditorLocalPatchReview(std::string_view answer,
                                                                                    const EditorLocalAgentPatchSource &source,
                                                                                    std::string_view project, std::uint64_t generation,
                                                                                    const EditorPatchDestination *destination = nullptr);
}  // namespace Horo::Editor
