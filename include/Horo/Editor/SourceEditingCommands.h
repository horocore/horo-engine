#pragma once

/** @file SourceEditingCommands.h
 * @brief GUI-independent source interaction planning with exact UTF-8 document offsets.
 */
#include "Horo/Editor/SourceDocumentService.h"

#include <variant>

namespace Horo::Editor {
    /** @brief Owning atomic edit proposal; apply through the document service with its captured revision. */
    struct SourceEditingPlan final {
        std::uint64_t revision{};
        std::size_t offset{};
        std::size_t eraseBytes{};
        std::string insert;
        SourceEditingView before;
        SourceEditingView after;
        /** @brief Borrows this plan synchronously. @return Revision-fenced patch including history view states. */
        [[nodiscard]] SourceTextEdit Edit() const noexcept;
    };

    /** @brief Owning semantic edit submission from any presentation or service client. */
    struct SourceEditSubmission final {
        DocumentInstanceId instance;
        SourceEditingPlan plan;
    };
    /** @brief History replay direction, owned by the document service rather than a widget. */
    enum class SourceHistoryDirection {
        Undo,
        Redo
    };

    /** @brief Exact document and revision fence for a history command. */
    struct SourceHistoryRequest final {
        DocumentInstanceId instance;
        std::uint64_t expectedRevision{};
        SourceHistoryDirection direction{SourceHistoryDirection::Undo};
    };

    /** @brief Closed set of owning source mutations routed through the workspace controller. */
    using SourceEditingCommand = std::variant<SourceEditSubmission, SourceHistoryRequest>;

    /** @brief Authoritative successful publication, with optional originating-view restoration metadata.
     * @details Consume a restoration only once for this snapshot revision and matching live view ID.
     * Errors retain the prior document publication; the host displays them without assuming success.
     */
    struct SourceEditingPublication final {
        SourceDocumentSnapshot snapshot;
        std::optional<SourceEditingView> restoration;
        SourceHistoryState history;
    };

    /** @brief Applies a typed source command through the sole document/history authority.
     * @param documents Project-scoped source owner. @param command Owning revision-fenced edit or replay request.
     * @param cancellation Cooperative observer. @return Complete publication or preserved document-domain failure.
     */
    [[nodiscard]] Result<SourceEditingPublication> SubmitSourceEditingCommand(SourceDocumentService &documents,
                                                                              const SourceEditingCommand &command,
                                                                              CancellationToken cancellation = {});
    /** @brief Literal UTF-8 search direction; wrap searches the complementary document interval once. */
    enum class SourceSearchDirection {
        Forward,
        Backward
    };
    /** @brief Validated selection replacement, including clipboard/IME text as one history entry.
     * @param snapshot Current source lease. @param view View-local byte selection and scroll.
     * @param text Exact UTF-8 insertion. @return Atomic proposal or invalid/oversized input error.
     */
    [[nodiscard]] Result<SourceEditingPlan> PlanSourceReplacement(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                                  std::string_view text);
    /** @brief Replaces the selection with the local newline family and current line's leading indentation.
     * @param snapshot Current source. @param view Exact view selection. @return One atomic auto-indented newline proposal.
     */
    [[nodiscard]] Result<SourceEditingPlan> PlanSourceNewline(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view);
    /** @brief Indents or unindents all selected logical lines, preserving BOM and newline families.
     * @param snapshot Current source. @param view Selection; a selection ending at line start excludes that line.
     * @param width Spaces per indentation step, from one through eight. @param outdent Remove one tab or up to width spaces.
     * @return One atomic proposal and adjusted selection; a collapsed caret remains collapsed.
     */
    [[nodiscard]] Result<SourceEditingPlan> PlanSourceIndentation(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                                  unsigned width, bool outdent);
    /** @brief Finds one literal, case-sensitive UTF-8 match without allocating a match list.
     * @param snapshot Current source. @param view Starting selection. @param query Nonempty valid UTF-8 literal.
     * @param direction Search order. @param wrap Search the complementary interval once.
     * @return Matching selection preserving scroll, or empty when no valid match/request exists.
     */
    [[nodiscard]] std::optional<SourceEditingView> FindSourceText(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                                  std::string_view query, SourceSearchDirection direction, bool wrap);
    /** @brief Plans bounded nonoverlapping literal replacements as one undo step, without mutating source on failure.
     * @param snapshot Current source. @param view Current view. @param query Nonempty UTF-8 literal.
     * @param replacement Valid UTF-8 insertion. @return Atomic proposal limited to the document byte ceiling.
     */
    [[nodiscard]] Result<SourceEditingPlan> PlanSourceReplaceAll(const SourceDocumentSnapshot &snapshot, const SourceEditingView &view,
                                                                 std::string_view query, std::string_view replacement);
    /** @brief Locates a one-based logical line and Unicode scalar column; oversized values clamp to end.
     * @param snapshot Current source. @param line One-based line. @param column One-based scalar column.
     * @return Collapsed selection at a scalar boundary after the BOM, or empty for an invalid request.
     */
    [[nodiscard]] std::optional<SourceEditingView> NavigateSourceLine(const SourceDocumentSnapshot &snapshot, std::size_t line,
                                                                      std::size_t column = 1);
}  // namespace Horo::Editor
