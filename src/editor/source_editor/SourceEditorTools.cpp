#include "SourceEditorTools.h"

#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/input/EditorInputActions.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Resolves all visible copy through the owning localization catalog. */
        const std::string &Text(const EditorGuiContext &context, const char *key) {
            return context.localization.Get("editor", key);
        }

        /** @brief Wraps actions from measured bounds, retaining the current editor typography at narrow widths. */
        bool Button(const EditorGuiContext &context, const char *key, const bool enabled = true) {
            const auto &label = Text(context, key);
            const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
            if (const float width = [&context, &metrics, &label] {
                const Theme::ScopedTextStyle style{context.theme.fonts.sans, metrics.fontSize, Theme::FontPx::Sans};
                return ImGui::CalcTextSize(label.c_str()).x + metrics.paddingX * 2;
            }(); ImGui::GetContentRegionAvail().x < width)
                ImGui::NewLine();
            const bool clicked = Ui::Button(
                {.label = label.c_str(), .variant = Ui::ButtonVariant::Secondary, .enabled = enabled, .font = context.theme.fonts.sans});
            ImGui::SameLine();
            return clicked;
        }

        /** @brief Uses borderless shared icon actions on the single source toolbar row. */
        bool ToolbarAction(const EditorGuiContext &context, const char *key, const Ui::IconButtonGlyph glyph, const bool enabled = true) {
            const bool clicked =
                Ui::IconButton({.id = key, .glyph = glyph, .tooltip = Text(context, key).c_str(), .enabled = enabled, .borderless = true});
            ImGui::SameLine();
            return clicked;
        }
    }  // namespace

    /** @copydoc SourceEditorTools::TakeFocusRequest */
    bool SourceEditorTools::TakeFocusRequest() noexcept {
        return std::exchange(focusSource_, false);
    }

    /** @copydoc SourceEditorTools::Find */
    void SourceEditorTools::Find(SourceEditorWidget &widget, const SourceDocumentSnapshot &source, const SourceSearchDirection direction) {
        if (const auto match = FindSourceText(source, widget.View(), query_, direction, true)) {
            static_cast<void>(widget.Select({match->anchor, match->caret}));
            focusSource_ = true;
            status_ = Status::None;
        } else {
            status_ = Status::NoMatch;
        }
    }

    /** @copydoc SourceEditorTools::Replacement */
    std::optional<SourceEditingCommand> SourceEditorTools::Replacement(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                                       const bool all) {
        auto view = widget.View();
        const auto low = std::min(view.anchor, view.caret);
        if (const auto high = std::max(view.anchor, view.caret); !all && source.Text().substr(low, high - low) != query_) {
            Find(widget, source, SourceSearchDirection::Forward);
            return std::nullopt;
        }
        auto plan = all ? PlanSourceReplaceAll(source, view, query_, replacement_) : PlanSourceReplacement(source, view, replacement_);
        if (plan.HasError()) {
            status_ = Status::Invalid;
            return std::nullopt;
        }
        status_ = Status::None;
        focusSource_ = true;
        return SourceEditSubmission{source.Identity().instance, std::move(plan).Value()};
    }

    /** @copydoc SourceEditorTools::Search */
    std::optional<SourceEditingCommand> SourceEditorTools::Search(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                                  const EditorGuiContext &context) {
        if (!searchOpen_)
            return std::nullopt;
        const auto &fonts = context.theme.fonts;
        if (std::exchange(focusSearch_, false))
            ImGui::SetKeyboardFocusHere();
        static_cast<void>(Ui::InputTextControl("##source-find-query", query_, 4097, fonts,
                                               {.hint = Text(context, "workspace.source_edit.query").c_str()}));
        ownsInput_ = ownsInput_ || ImGui::IsItemActive();
        static_cast<void>(Ui::InputTextControl("##source-replacement", replacement_, 4097, fonts,
                                               {.hint = Text(context, "workspace.source_edit.replacement").c_str()}));
        ownsInput_ = ownsInput_ || ImGui::IsItemActive();
        if (Button(context, "workspace.source_edit.next", !query_.empty()))
            Find(widget, source, SourceSearchDirection::Forward);
        if (Button(context, "workspace.source_edit.previous", !query_.empty()))
            Find(widget, source, SourceSearchDirection::Backward);
        std::optional<SourceEditingCommand> intent;
        if (Button(context, "workspace.source_edit.replace", !query_.empty()))
            intent = Replacement(widget, source, false);
        if (Button(context, "workspace.source_edit.replace_all", !query_.empty()))
            intent = Replacement(widget, source, true);
        if (Button(context, "workspace.source_edit.close_search")) {
            searchOpen_ = false;
            focusSource_ = true;
        }
        ImGui::NewLine();
        return intent;
    }

    /** @copydoc SourceEditorTools::Navigate */
    void SourceEditorTools::Navigate(SourceEditorWidget &widget, const SourceDocumentSnapshot &source, const EditorGuiContext &context) {
        if (!lineOpen_)
            return;
        if (std::exchange(focusLine_, false))
            ImGui::SetKeyboardFocusHere();
        static_cast<void>(Ui::InputTextControl("##source-line-number", line_, 32, context.theme.fonts,
                                               {.hint = Text(context, "workspace.source_edit.line").c_str()}));
        ownsInput_ = ownsInput_ || ImGui::IsItemActive();
        ImGui::PushID("navigate-apply");
        if (Button(context, "workspace.source_edit.go_to")) {
            std::size_t line = 0;
            const auto parsed = std::from_chars(line_.data(), line_.data() + line_.size(), line);
            const auto position =
                parsed.ec == std::errc{} && parsed.ptr == line_.data() + line_.size() ? NavigateSourceLine(source, line) : std::nullopt;
            if (position) {
                static_cast<void>(widget.Select({position->anchor, position->caret}));
                status_ = Status::None;
                focusSource_ = true;
                lineOpen_ = false;
            } else {
                status_ = Status::Invalid;
            }
        }
        ImGui::PopID();
        ImGui::NewLine();
    }

    /** @copydoc SourceEditorTools::StatusText */
    void SourceEditorTools::StatusText(const SourceHistoryState &history, const std::optional<Result<SourceEditingPublication>> &outcome,
                                       const EditorGuiContext &context) const {
        const char *status = nullptr;
        if (history.boundary == SourceHistoryBoundary::Evicted)
            status = "workspace.source_edit.history_evicted";
        else if (history.boundary == SourceHistoryBoundary::OversizedEdit)
            status = "workspace.source_edit.history_oversized";
        if (status)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, status), Theme::Muted(), context.theme.fonts);
        if (status_ == Status::NoMatch)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, Text(context, "workspace.source_edit.no_match"), Theme::Muted(),
                            context.theme.fonts);
        if (status_ == Status::Invalid || (outcome && outcome->HasError()))
            Ui::ErrorText(Text(context, "workspace.source_edit.invalid").c_str(), context.theme.fonts);
    }

    /** @copydoc SourceEditorTools::Draw */
    std::optional<SourceEditingCommand> SourceEditorTools::Draw(SourceEditorWidget &widget, const SourceDocumentSnapshot &source,
                                                                const SourceHistoryState &history,
                                                                const std::optional<Result<SourceEditingPublication>> &outcome,
                                                                const EditorGuiContext &context) {
        ownsInput_ = false;
        handledShortcut_ = false;
        const auto identity = widget.Identity().value;
        ImGui::PushID(static_cast<int>(identity >> 32));
        ImGui::PushID(static_cast<int>(identity & 0xffffffffU));
        std::optional<SourceEditingCommand> intent;
        const auto &profile = context.settings.settings.keymap.profile;
        const bool acceptsShortcuts = widget.OwnsInput() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                      !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const auto pressed = [&](const char *action, const bool repeat = false) {
            const bool matched = acceptsShortcuts && Ui::ShortcutPressed(EditorActionBindings(action, profile), repeat);
            if (matched) {
                handledShortcut_ = true;
                ImGui::GetIO().InputQueueCharacters.resize(0);
            }
            return matched;
        };
        static_cast<void>(pressed(kActionSave));
        if (pressed(kActionSourceZoomIn, true))
            widget.SetFontZoom(widget.FontZoom() + 1);
        if (pressed(kActionSourceZoomOut, true))
            widget.SetFontZoom(widget.FontZoom() - 1);
        if (pressed(kActionSourceZoomReset))
            widget.SetFontZoom(0);
        if (pressed(kActionUndo) && history.undoEntries != 0)
            intent = SourceHistoryRequest{source.Identity().instance, source.Revision(), SourceHistoryDirection::Undo};
        if (pressed(kActionRedo) && history.redoEntries != 0)
            intent = SourceHistoryRequest{source.Identity().instance, source.Revision(), SourceHistoryDirection::Redo};
        if (pressed(kActionSourceFind)) {
            searchOpen_ = !searchOpen_;
            focusSearch_ = searchOpen_;
        }
        if (pressed(kActionSourceGoToLine)) {
            lineOpen_ = !lineOpen_;
            focusLine_ = lineOpen_;
        }
        if (ToolbarAction(context, "workspace.source_edit.undo", Ui::IconButtonGlyph::Undo, history.undoEntries != 0))
            intent = SourceHistoryRequest{source.Identity().instance, source.Revision(), SourceHistoryDirection::Undo};
        if (ToolbarAction(context, "workspace.source_edit.redo", Ui::IconButtonGlyph::Redo, history.redoEntries != 0))
            intent = SourceHistoryRequest{source.Identity().instance, source.Revision(), SourceHistoryDirection::Redo};
        if (ToolbarAction(context, "workspace.source_edit.find", Ui::IconButtonGlyph::Search)) {
            searchOpen_ = !searchOpen_;
            focusSearch_ = searchOpen_;
        }
        if (ToolbarAction(context, "workspace.source_edit.go_to", Ui::IconButtonGlyph::GoToLine)) {
            lineOpen_ = !lineOpen_;
            focusLine_ = lineOpen_;
        }
        ImGui::NewLine();
        if (auto search = Search(widget, source, context))
            intent = std::move(search);
        Navigate(widget, source, context);
        StatusText(history, outcome, context);
        ImGui::PopID();
        ImGui::PopID();
        return intent;
    }
}  // namespace Horo::Editor
