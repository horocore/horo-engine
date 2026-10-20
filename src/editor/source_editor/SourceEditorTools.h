#pragma once

/** @file SourceEditorTools.h
 * @brief View-local controls translating source editing, search and navigation into canonical commands.
 */
#include "Horo/Editor/EditorGuiContext.h"
#include "SourceEditorWidget.h"

namespace Horo::Editor {
    /** @brief Owns only one view's search drafts and transient status, never document or language state. */
    class SourceEditorTools final {
    public:
        /** @brief Draws the visible source tools through shared editor controls.
         * @param widget Target presentation. @param source Authoritative bytes. @param history Document history availability.
         * @param outcome Latest controller publication or failure. @param context Localization and design tokens.
         * @return One canonical mutation intent, or none for presentation-only search/navigation.
         */
        [[nodiscard]] std::optional<SourceEditingCommand> Draw(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                               const SourceHistoryState &history,
                                                               const std::optional<Result<SourceEditingPublication>> &outcome,
                                                               const EditorGuiContext &context);
        /** @brief Consumes a one-frame focus handoff after navigation. @return Whether to focus the source surface. */
        [[nodiscard]] bool TakeFocusRequest() noexcept;

    private:
        enum class Status {
            None,
            NoMatch,
            Invalid
        };
        std::string query_;
        std::string replacement_;
        std::string line_{"1"};
        bool searchOpen_{};
        bool lineOpen_{};
        bool focusSource_{};
        Status status_{Status::None};
        void Find(SourceEditorWidget &widget, const SourceDocumentSnapshot &source, SourceSearchDirection direction);
        [[nodiscard]] std::optional<SourceEditingCommand> Replacement(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                                      bool all);
        [[nodiscard]] std::optional<SourceEditingCommand> Search(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                                 const EditorGuiContext &context);
        void StatusText(const SourceHistoryState &history, const std::optional<Result<SourceEditingPublication>> &outcome,
                        const EditorGuiContext &context) const;
        void Navigate(SourceEditorWidget &widget, const SourceDocumentSnapshot &source, const EditorGuiContext &context);
    };
}  // namespace Horo::Editor
