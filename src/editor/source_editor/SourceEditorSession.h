#pragma once

/** @file SourceEditorSession.h
 * @brief Explicit owner-session fence for source presentation caches and never-reused view identities.
 */
#include "Horo/Editor/WorkspacePanelHost.h"
#include "SourceEditorTools.h"
#include "SourceSyntaxCache.h"

#include <map>

namespace Horo::Editor {
    /** @brief One independent source presentation; no document or language authority. */
    struct SourceEditorView final {
        explicit SourceEditorView(SourceEditingViewId identity) : widget(identity) {}

        /** @brief Reconciles one publication into independent interaction and lexical projections.
         * @param source Current authoritative source publication.
         */
        void Publish(const SourceDocumentSnapshot &source);

        SourceEditorWidget widget;
        SourceEditorTools tools;
        SourceSyntaxCache syntax;
        std::shared_ptr<const SourceReviewOverlay> pendingReview;
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
