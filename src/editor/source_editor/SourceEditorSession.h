#pragma once

/** @file SourceEditorSession.h
 * @brief Explicit owner-session fence for source presentation caches and never-reused view identities.
 */
#include "Horo/Editor/WorkspacePanelHost.h"
#include "SourceEditorTools.h"
#include "SourceSyntaxCache.h"

#include <map>

namespace Horo::Editor {
    namespace Language {
        struct LanguageResult;
        struct LanguageSourceFocus;
    }  // namespace Language

    /** @brief Error and warning counts for the exact current source behind a review. */
    struct SourceReviewDiagnosticCounts final {
        std::size_t errors{};
        std::size_t warnings{};
    };

    /** @brief One independent source presentation; no document or language authority. */
    struct SourceEditorView final {
        explicit SourceEditorView(SourceEditingViewId identity) : widget(identity) {}

        /** @brief Reconciles one publication into independent interaction and lexical projections.
         * @param source Current authoritative source publication.
         */
        void Publish(const SourceDocumentSnapshot &source);
        /** @brief Presents diagnostics for the exact source publication, or clears retired decorations. */
        void PublishDiagnostics(const SourceDocumentSnapshot &source, std::shared_ptr<const Language::LanguageResult> result);
        /** @brief Returns read-only counts for the supplied current source.
         * @param source The current authoritative source publication.
         * @return Counts when that exact revision has language evidence.
         */
        [[nodiscard]] std::optional<SourceReviewDiagnosticCounts> CurrentDiagnostics(const SourceDocumentSnapshot &source) const;
        /** @brief Selects an exact language destination once in this presentation. */
        [[nodiscard]] bool ApplyLanguageFocus(const SourceDocumentSnapshot &source,
                                              const std::shared_ptr<const Language::LanguageSourceFocus> &focus);

        SourceEditorWidget widget;
        SourceEditorTools tools;
        SourceSyntaxCache syntax;
        std::shared_ptr<const SourceReviewOverlay> pendingReview;
        std::shared_ptr<const Language::LanguageResult> diagnostics;
        std::weak_ptr<const Language::LanguageSourceFocus> lastLanguageFocus;

    private:
        std::optional<SourceReviewDiagnosticCounts> diagnosticCounts_;
    };

    /** @brief Project-owner-scoped presentations with a monotonic host-lifetime identity allocator. */
    class SourceEditorSession final {
    public:
        SourceEditorSession() = default;
        SourceEditorSession(const SourceEditorSession &) = delete;
        SourceEditorSession &operator=(const SourceEditorSession &) = delete;
        /** @brief Obtains one live document's view. @param instance Owner-issued document instance.
         * @return Presentation, or null when its typed identity is inert or view IDs are exhausted.
         */
        [[nodiscard]] SourceEditorView *Acquire(DocumentInstanceId instance);
        /** @brief Queues a prepared overlay for the source tab's next presentation. */
        [[nodiscard]] bool StageReview(std::shared_ptr<const SourceReviewOverlay> overlay);
        /** @brief Observes a range in an existing widget only. No presentation is created on a miss. */
        [[nodiscard]] std::optional<SourceContextRange> ReadSelection(const SourceDocumentSnapshot &snapshot) const;
        /** @brief Removes only explicitly closed document presentations. @param tabs Current host-owned tab inventory. */
        void Retain(std::span<const WorkspaceDocumentTab> tabs);
        /** @brief Releases an old owner's widgets, tools and focus without reusing any allocated view IDs. */
        void Reset() noexcept;
        /** @brief Checks the last source surface focus. @param instance Candidate source document. @return Whether to hand focus to it. */
        [[nodiscard]] bool NeedsFocus(DocumentInstanceId instance) const noexcept;
        /** @brief Records the source surface presented this frame. @param instance Exact live document. */
        void Presented(DocumentInstanceId instance) noexcept;
        /** @brief Releases source focus when a different document kind is presented. */
        void ClearFocus() noexcept;

    private:
        std::map<std::uint64_t, SourceEditorView> views_;
        std::uint64_t nextViewId_{1};
        std::optional<DocumentInstanceId> focused_;
    };
}  // namespace Horo::Editor
