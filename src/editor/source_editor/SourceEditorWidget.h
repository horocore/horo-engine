#pragma once

/** @file SourceEditorWidget.h
 * @brief GUI-private, replaceable source interaction boundary. All offsets are exact document UTF-8 bytes.
 */
#include "Horo/Editor/SourceEditingCommands.h"

#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace Horo::Editor {
    /** @brief Half-open, revision-relative document byte selection. */
    struct SourceWidgetSelection final {
        std::size_t anchor{};
        std::size_t caret{};
        auto operator<=>(const SourceWidgetSelection &) const = default;
    };

    /** @brief Presentation-only diagnostic style; language ownership stays outside the widget. */
    enum class SourceWidgetDecorationKind {
        Error,
        Warning,
        Information
    };

    /** @brief Bounded external decoration at an exact document range. */
    struct SourceWidgetDecoration final {
        std::size_t begin{};
        std::size_t end{};
        SourceWidgetDecorationKind kind{SourceWidgetDecorationKind::Information};
        std::string description;
    };

    /** @brief Frame-local logical extent and host focus policy, independent of native handles. */
    struct SourceWidgetFrame final {
        float width{};
        float height{};
        float dpiScale{1.0F};
        bool requestFocus{};
        bool inputEnabled{true};
    };

    /** @brief Editor-thread RAII presentation mirror; never reads or writes files or owns a document/language service.
     * @details Each instance owns its own interaction state. Publish the authoritative snapshot before every Draw,
     * including after rejected edits. Retains no queued edits across frames. Destruction needs no GUI context.
     */
    class SourceEditorWidget final {
    public:
        /** @brief Creates a view mirror with a host-issued, session-unique nonzero identity.
         * @param viewIdentity Never reused within the document owner's project session, including after close/reopen.
         */
        explicit SourceEditorWidget(SourceEditingViewId viewIdentity);
        ~SourceEditorWidget();
        SourceEditorWidget(const SourceEditorWidget &) = delete;
        SourceEditorWidget &operator=(const SourceEditorWidget &) = delete;
        /** @brief Reconciles a document publication; rejection/external changes reset obsolete interaction caches.
         * @param snapshot Authoritative immutable lease; an inert lease detaches the widget.
         * @pre An initialized GUI context with a populated font atlas exists; its first frame may start later.
         */
        void Publish(const SourceDocumentSnapshot &snapshot);
        /** @brief Draws inside the current GUI window and emits at most one revision-fenced operation.
         * @param frame Host extent, scale and focus permission. @return Owned edit intent or no text change.
         */
        [[nodiscard]] std::optional<SourceEditingCommand> Draw(const SourceWidgetFrame &frame);
        /** @brief Returns exact document-byte selection. @return Empty selection if detached. */
        [[nodiscard]] SourceWidgetSelection Selection() const;
        /** @brief Captures this view's current presentation state for semantic editing/search commands.
         * @return Exact document byte offsets, scroll and host-issued view identity.
         */
        [[nodiscard]] SourceEditingView View() const;
        /** @brief Returns the host-issued presentation identity without inspecting source bytes. @return Session-unique view ID. */
        [[nodiscard]] SourceEditingViewId Identity() const noexcept;
        /** @brief Reports current-frame keyboard/text ownership, including no-op input on a newly focused view.
         * @return True only when the actual enabled surface owns this frame's input.
         */
        [[nodiscard]] bool OwnsInput() const noexcept;
        /** @brief Selects a validated document range. @param selection UTF-8/BOM/newline-aligned boundaries.
         * @return False without mutation for an invalid range or a detached widget.
         */
        [[nodiscard]] bool Select(SourceWidgetSelection selection);
        /** @brief Applies a history view result only to this presentation instance and exact document publication.
         * @param publication Authoritative history result with originating view metadata. @return True only when consumed by this view.
         */
        [[nodiscard]] bool Restore(const SourceEditingPublication &publication);
        /** @brief Scrolls this instance through the same path as a mouse wheel. @param lines Signed logical display-line delta; positive
         * scrolls down. */
        void Scroll(float lines);
        /** @brief Validates a complete external decoration projection before replacing it for the exact current revision.
         * @param revision Published document revision. @param decorations At most 4096 valid document ranges.
         * @return False without mutation for stale, invalid or oversized input.
         */
        [[nodiscard]] bool Decorate(std::uint64_t revision, std::span<const SourceWidgetDecoration> decorations);

    private:
        struct Storage;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Editor
