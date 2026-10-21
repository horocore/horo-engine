#include "SourceEditorWidget.h"

#include "Horo/Foundation/Utf8.h"
#include "SourceWidgetPlatform.h"
#include "SourceWidgetText.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstring>
#include <imgui_internal.h>
#include <zep/mode_standard.h>
#include <zep/tab_window.h>
#include <zep/window.h>

namespace Horo::Editor {
    using SourceWidgetText::AdmittedText;
    using SourceWidgetText::AuthoredText;
    using SourceWidgetText::Boundary;
    using SourceWidgetText::DisplayOffset;
    using SourceWidgetText::DisplayText;
    using SourceWidgetText::RawOffset;

    namespace {
        /** @brief Rejects non-finite geometry before it reaches the GUI layout engine. */
        bool ValidFrame(const SourceWidgetFrame &frame) {
            return std::isfinite(frame.width) && std::isfinite(frame.height) && frame.width > 0 && frame.height > 0 &&
                   std::isfinite(frame.dpiScale) && frame.dpiScale > 0;
        }

        /** @brief Validates one external marker before replacing any existing decoration. */
        bool ValidDecoration(const SourceDocumentSnapshot &source, const SourceWidgetDecoration &decoration) {
            return decoration.begin <= decoration.end && Boundary(source, decoration.begin) && Boundary(source, decoration.end) &&
                   decoration.description.size() <= 4096 && IsValidUtf8ScalarSequence(decoration.description);
        }

        /** @brief Keeps semantic marker color selection outside range validation. */
        Zep::ThemeColor DecorationColor(const SourceWidgetDecorationKind kind) {
            if (kind == SourceWidgetDecorationKind::Error)
                return Zep::ThemeColor::Error;
            return kind == SourceWidgetDecorationKind::Warning ? Zep::ThemeColor::Warning : Zep::ThemeColor::Info;
        }
    }  // namespace

    /** @brief One editor per view; Zep owns its components, callbacks, display and denied filesystem. */
    struct SourceEditorWidget::Storage final {
        SourceDocumentSnapshot source;
        std::unique_ptr<Zep::ZepEditor> editor;
        Zep::ZepBuffer *buffer{};
        Zep::ZepWindow *window{};
        std::string mirror;
        std::uint64_t update{};
        bool pending{};
        bool pointerCaptured{};
        std::uint64_t restoredRevision{};
        ImGuiID inputId{};
        float scroll{};
        std::optional<float> pendingViewport;
        bool ownsInput{};
        SourceEditingView beforeView;
        std::optional<SourceHistoryRequest> history;
        std::optional<SourceEditSubmission> planned;
        SourceEditingViewId viewIdentity{};
        std::string inputName;

        /** @brief Captures the actual camera after keyboard reveal/layout, not an accumulated input delta. */
        float ScrollPosition() const {
            return window ? window->GetHoroViewportLines() : 0.0F;
        }

        /** @brief Retains presentation state only for the same live document instance. */
        std::optional<SourceEditingView> PreviousView(const SourceDocumentSnapshot &next) const {
            if (!source.Revision() || source.Identity().instance != next.Identity().instance)
                return std::nullopt;
            const auto selected = CaptureSelection();
            return SourceEditingView{selected.anchor, selected.caret, ScrollPosition(), viewIdentity};
        }

        /** @brief Recognizes an accepted widget publication without resetting independent interaction state. */
        bool Matches(const SourceDocumentSnapshot &next, const std::string &display) const {
            return editor && source.Revision() && source.Identity().instance == next.Identity().instance &&
                   display == buffer->GetBufferText(buffer->Begin(), buffer->End());
        }

        /** @brief Rebuilds only for a new identity or an authoritative replacement/rejection. */
        void Reset(const std::string &text) {
            auto candidate = MakeSourceWidgetEditor();
            candidate->RegisterGlobalMode(std::make_shared<WidgetMode>(*candidate));
            candidate->SetGlobalMode(Zep::ZepMode_Standard::StaticName());
            // A fixed extension-free name never chooses a language service or syntax provider.
            auto *nextBuffer = candidate->InitWithText("source", text);
            nextBuffer->SetSyntax({});
            auto *nextWindow = candidate->GetActiveTabWindow()->GetActiveWindow();
            editor = std::move(candidate);
            buffer = nextBuffer;
            window = nextWindow;
            update = buffer->GetUpdateCount();
        }

        /** @brief Inserts committed UTF-8 through Zep's command path, bypassing its eight-bit key conversion. */
        void Insert(const std::string &text) {
            if (text.empty() || !AdmittedText(text))
                return;
            auto start = window->GetBufferCursor();
            auto end = start;
            if (buffer->HasSelection()) {
                const auto selection = buffer->GetInclusiveSelection();
                start = selection.first;
                end = selection.second.Peek(1);
            }
            Zep::ChangeRecord change;
            if (start != end)
                buffer->Delete(start, end, change);
            buffer->Insert(start, text, change);
            window->SetBufferCursor(Zep::GlyphIterator(buffer, static_cast<unsigned long>(start.Index()) + text.size()));
            buffer->GetMode()->SwitchMode(Zep::EditorMode::Insert);
            buffer->ClearSelection();
        }

        /** @brief Routes navigation only through Zep's standard selection/movement semantics. */
        void Navigation(const std::uint32_t modifiers) {
            constexpr std::array keys{std::pair{ImGuiKey_LeftArrow, Zep::ExtKeys::LEFT},
                                      std::pair{ImGuiKey_RightArrow, Zep::ExtKeys::RIGHT},
                                      std::pair{ImGuiKey_UpArrow, Zep::ExtKeys::UP},
                                      std::pair{ImGuiKey_DownArrow, Zep::ExtKeys::DOWN},
                                      std::pair{ImGuiKey_Home, Zep::ExtKeys::HOME},
                                      std::pair{ImGuiKey_End, Zep::ExtKeys::END},
                                      std::pair{ImGuiKey_PageUp, Zep::ExtKeys::PAGEUP},
                                      std::pair{ImGuiKey_PageDown, Zep::ExtKeys::PAGEDOWN},
                                      std::pair{ImGuiKey_Backspace, Zep::ExtKeys::BACKSPACE},
                                      std::pair{ImGuiKey_Delete, Zep::ExtKeys::DEL},
                                      std::pair{ImGuiKey_Escape, Zep::ExtKeys::ESCAPE}};
            for (const auto &[key, zepKey] : keys) {
                ImGui::SetKeyOwner(key, inputId);
                if (ImGui::IsKeyPressed(key, ImGuiInputFlags_Repeat, inputId))
                    buffer->GetMode()->AddKeyPress(zepKey, modifiers);
            }
        }

        /** @brief Copies exact selected document bytes, leaving empty selections and rejected clipboard data inert. */
        void Clipboard(const ImGuiKey key) {
            if (key == ImGuiKey_V) {
                const char *bytes = ImGui::GetClipboardText();
                if (bytes && AdmittedText(bytes))
                    Insert(DisplayText(bytes));
                return;
            }
            if (!buffer->HasSelection())
                return;
            const auto selected = CaptureSelection();
            const auto low = std::min(selected.anchor, selected.caret);
            const auto high = std::max(selected.anchor, selected.caret);
            const std::string text{source.Text().substr(low, high - low)};
            ImGui::SetClipboardText(text.c_str());
            if (key == ImGuiKey_X) {
                const auto range = buffer->GetInclusiveSelection();
                Zep::ChangeRecord change;
                buffer->Delete(range.first, range.second.Peek(1), change);
                window->SetBufferCursor(range.first);
                buffer->GetMode()->SwitchMode(Zep::EditorMode::Insert);
                buffer->ClearSelection();
            }
        }

        /** @brief Converts host history shortcuts outward while using Zep only for clipboard/selection. */
        void Shortcuts(const std::uint32_t modifiers) {
            const auto &io = ImGui::GetIO();
            if (ImGui::IsKeyPressed(ImGuiKey_Z) || ImGui::IsKeyPressed(ImGuiKey_Y)) {
                const bool redo = io.KeyShift || ImGui::IsKeyPressed(ImGuiKey_Y);
                history = SourceHistoryRequest{source.Identity().instance, source.Revision(),
                                               redo ? SourceHistoryDirection::Redo : SourceHistoryDirection::Undo};
            }
            constexpr std::array commands{ImGuiKey_A, ImGuiKey_C, ImGuiKey_V, ImGuiKey_X};
            for (const auto key : commands) {
                ImGui::SetKeyOwner(key, inputId);
                if (!ImGui::IsKeyPressed(key, ImGuiInputFlags_None, inputId))
                    continue;
                if (key == ImGuiKey_A)
                    buffer->GetMode()->AddKeyPress('a', modifiers);
                else
                    Clipboard(key);
            }
        }

        /** @brief Inserts to the next four-column stop at a caret; selections and Shift-Tab use line indentation. */
        Result<SourceEditingPlan> TabPlan(const SourceEditingView &view, const bool outdent) const {
            constexpr unsigned indentationWidth = 4;
            if (outdent || view.anchor != view.caret)
                return PlanSourceIndentation(source, view, indentationWidth, outdent);
            const auto text = source.Text();
            const auto before = text.substr(0, view.caret);
            const auto newline = before.find_last_of("\r\n");
            std::size_t start = newline == std::string_view::npos ? (source.Metadata().utf8Bom ? 3 : 0) : newline + 1;
            std::size_t column = 0;
            for (; start < view.caret; ++start) {
                if (text[start] == '\t')
                    column += indentationWidth - column % indentationWidth;
                else if ((static_cast<unsigned char>(text[start]) & 0xC0U) != 0x80U)
                    ++column;
            }
            return PlanSourceReplacement(source, view, std::string(indentationWidth - column % indentationWidth, ' '));
        }

        /** @brief Routes newline and indentation through the canonical source planners. */
        void SemanticKeys() {
            const auto &io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size == 0 && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Tab))) {
                const auto selection = CaptureSelection();
                const SourceEditingView view{selection.anchor, selection.caret, ScrollPosition(), viewIdentity};
                if (ImGui::IsKeyPressed(ImGuiKey_Enter)) {
                    if (auto plan = PlanSourceNewline(source, view); plan.HasValue())
                        planned = SourceEditSubmission{source.Identity().instance, std::move(plan).Value()};
                } else if (ImGui::IsKeyPressed(ImGuiKey_Tab)) {
                    if (auto plan = TabPlan(view, io.KeyShift); plan.HasValue())
                        planned = SourceEditSubmission{source.Identity().instance, std::move(plan).Value()};
                }
            }
        }

        /** @brief Delivers committed Unicode scalars without Zep's eight-bit key conversion. */
        void CommittedText() {
            const auto &io = ImGui::GetIO();
            SemanticKeys();
            std::string committed;
            for (const ImWchar scalar : io.InputQueueCharacters) {
                if (scalar < 32 || scalar == 127)
                    continue;
                std::array<char, 5> bytes{};
                ImTextCharToUtf8(bytes.data(), scalar);
                committed += bytes.data();
            }
            Insert(committed);
            ImGui::GetIO().InputQueueCharacters.resize(0);
        }

        /** @brief Routes only widget keys; save and other workspace shortcuts stay outside the widget. */
        void Keyboard() {
            const auto &io = ImGui::GetIO();
            const bool command = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
            std::uint32_t modifiers = io.KeyShift ? Zep::ModifierKey::Shift : 0;
            if (command)
                modifiers |= Zep::ModifierKey::Ctrl;
            if (io.KeyAlt)
                modifiers |= Zep::ModifierKey::Alt;
            Navigation(modifiers);
            if (command)
                Shortcuts(modifiers);
            else
                CommittedText();
        }

        /** @brief Owns a complete mouse selection gesture, including release outside the surface or focus cancellation. */
        void Pointer(const bool focused, const bool hovered) {
            const auto mouse = Zep::toNVec2f(ImGui::GetIO().MousePos);
            if (!focused) {
                if (pointerCaptured)
                    editor->OnMouseUp(mouse, Zep::ZepMouseButton::Left);
                pointerCaptured = false;
                return;
            }
            if (!hovered && !pointerCaptured)
                return;
            editor->OnMouseMove(mouse);
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                editor->OnMouseDown(mouse, Zep::ZepMouseButton::Left);
                pointerCaptured = true;
            }
            if (pointerCaptured && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                editor->OnMouseUp(mouse, Zep::ZepMouseButton::Left);
                pointerCaptured = false;
            }
        }

        /** @brief Keeps font-cache invalidation bounded to actual font or DPI changes. */
        WidgetDisplay &PrepareDisplay(const SourceWidgetFrame &frame, const ImVec2 origin, const ImVec2 extent) {
            auto &display = static_cast<WidgetDisplay &>(editor->GetDisplay());
            const float viewport = ScrollPosition();
            const auto generation = display.fontGeneration;
            bool changed = false;
            if (display.GetPixelScale().x != frame.dpiScale || display.GetPixelScale().y != frame.dpiScale) {
                display.SetPixelScale({frame.dpiScale, frame.dpiScale});
                changed = true;
            }
            const int fontPixels = static_cast<int>(ImGui::GetFontSize());
            for (int type = 0; type < static_cast<int>(Zep::ZepTextType::Count); ++type) {
                auto &font = display.GetFont(static_cast<Zep::ZepTextType>(type));
                if (font.GetPixelHeight() != fontPixels) {
                    font.SetPixelHeight(fontPixels);
                    changed = true;
                }
            }
            editor->SetDisplayRegion({origin.x, origin.y}, {origin.x + extent.x, origin.y + extent.y});
            if (changed || generation != display.fontGeneration) {
                window->DirtyLayout();
                window->SetHoroViewportLines(viewport);
            }
            UpdateSourceWidgetTheme(*editor);
            return display;
        }

        /** @brief Captures exact byte coordinates only when an interaction needs them, avoiding idle frame scans. */
        SourceWidgetSelection CaptureSelection() const {
            if (!buffer)
                return {};
            if (!buffer->HasSelection()) {
                const auto cursor = RawOffset(source, static_cast<std::size_t>(window->GetBufferCursor().Index()));
                return {cursor, cursor};
            }
            const auto selection = buffer->GetInclusiveSelection();
            const auto low = RawOffset(source, static_cast<std::size_t>(selection.first.Index()));
            const auto high = RawOffset(source, static_cast<std::size_t>(selection.second.Peek(1).Index()));
            return window->GetBufferCursor().Index() <= selection.first.Index() ? SourceWidgetSelection{high, low}
                                                                                : SourceWidgetSelection{low, high};
        }

        /** @brief Recognizes text-producing input without scanning the document on navigation/idle frames. */
        bool HasTextInput() const {
            const auto &io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size > 0 || ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Tab))
                return true;
            const bool command = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
            return command && (ImGui::IsKeyPressed(ImGuiKey_X) || ImGui::IsKeyPressed(ImGuiKey_V));
        }

        /** @brief Acquires keyboard focus only from host handoff or a click in this enabled surface. */
        bool AcquireFocus(const bool enabled, const bool request, const bool hovered) {
            auto *guiWindow = ImGui::GetCurrentWindow();
            inputId = guiWindow->GetID("text-input");
            if (enabled && (request || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
                ImGui::FocusWindow(guiWindow);
                ImGui::SetActiveID(inputId, guiWindow);
            }
            return enabled && ImGui::IsWindowFocused() && (ImGui::GetActiveID() == 0 || ImGui::GetActiveID() == inputId);
        }

        /** @brief Captures only focused text/navigation input and claims the owning host's text-input channel. */
        void Input(const bool focused, const bool hovered) {
            ownsInput = focused;
            if (focused) {
                ImGui::SetActiveID(inputId, ImGui::GetCurrentWindow());
                ImGui::KeepAliveID(inputId);
                ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (HasTextInput()) {
                    const auto selected = CaptureSelection();
                    beforeView = {selected.anchor, selected.caret, ScrollPosition(), viewIdentity};
                }
                Keyboard();
            }
            Pointer(focused, hovered);
        }

        /** @brief Applies this view's bounded scroll intent without transferring ownership to the document. */
        void ApplyScroll(const bool hovered, const ImVec2, const ImVec2) {
            constexpr float wheelLines = 5.0F;
            if (hovered)
                scroll -= ImGui::GetIO().MouseWheel * wheelLines;
            if (scroll != 0) {
                window->SetHoroViewportLines(ScrollPosition() + scroll);
                scroll = 0;
            }
        }

        /** @brief Restores an absolute camera before this frame's new input and automatic caret reveal. */
        void RestoreViewport() {
            if (pendingViewport) {
                window->SetHoroViewportLines(*pendingViewport);
                pendingViewport.reset();
            }
        }

        /** @brief Anchors platform IME data to this focused surface and releases stale active input on handoff. */
        void Ime(const WidgetDisplay &display, const bool focused, const ImVec2 origin) const {
            if (focused) {
                auto &ime = ImGui::GetCurrentContext()->PlatformImeData;
                ime.WantVisible = true;
                ime.InputPos = display.cursor ? Zep::toImVec2(display.cursor->topLeftPx) : origin;
                ime.InputLineHeight = ImGui::GetFontSize();
            } else if (ImGui::GetActiveID() == inputId) {
                ImGui::ClearActiveID();
            }
        }

        /** @brief Produces a minimal scalar-aligned edit in the authoritative document coordinate system. */
        std::optional<SourceEditSubmission> CaptureEdit() {
            if (update == buffer->GetUpdateCount())
                return std::nullopt;
            update = buffer->GetUpdateCount();
            const auto next = buffer->GetBufferText(buffer->Begin(), buffer->End());
            if (next == mirror)
                return std::nullopt;
            const auto [prefix, suffix] = SourceWidgetText::Compare(mirror, next);
            const auto begin = RawOffset(source, prefix);
            const auto end = RawOffset(source, mirror.size() - suffix);
            pending = true;
            SourceEditingPlan plan{source.Revision(),
                                   begin,
                                   end - begin,
                                   AuthoredText(source, std::string_view{next}.substr(prefix, next.size() - prefix - suffix)),
                                   beforeView,
                                   {}};
            std::string candidate{source.Text()};
            candidate.replace(begin, end - begin, plan.insert);
            const auto caret = RawOffset(candidate, source.Metadata().utf8Bom, static_cast<std::size_t>(window->GetBufferCursor().Index()));
            plan.after = {caret, caret, ScrollPosition(), viewIdentity};
            return SourceEditSubmission{source.Identity().instance, std::move(plan)};
        }
    };

    /** @copydoc SourceEditorWidget::SourceEditorWidget */
    SourceEditorWidget::SourceEditorWidget(const SourceEditingViewId viewIdentity) : storage_(std::make_unique<Storage>()) {
        assert(viewIdentity.value != 0);
        storage_->viewIdentity = viewIdentity;
        storage_->inputName = "source-widget-" + std::to_string(viewIdentity.value);
    }

    SourceEditorWidget::~SourceEditorWidget() = default;

    /** @copydoc SourceEditorWidget::Publish */
    void SourceEditorWidget::Publish(const SourceDocumentSnapshot &snapshot) {
        auto &state = *storage_;
        if (snapshot.Revision() == 0) {
            const auto identity = state.viewIdentity;
            const auto name = state.inputName;
            storage_ = std::make_unique<Storage>();
            storage_->viewIdentity = identity;
            storage_->inputName = name;
            return;
        }
        if (!state.pending && state.source.SharesStateWith(snapshot))
            return;
        const auto text = snapshot.Text().substr(snapshot.Metadata().utf8Bom ? 3 : 0);
        const auto display = DisplayText(text);
        const auto previous = state.PreviousView(snapshot);
        const bool accepted = state.Matches(snapshot, display);
        if (!accepted) {
            state.Reset(display);
            state.scroll = 0;
            state.pendingViewport = previous ? std::optional<float>{previous->scrollLines} : std::nullopt;
        }
        // This path runs only on a publication change or a rejected edit; external decorations must be refreshed.
        state.buffer->ClearRangeMarkers(Zep::RangeMarkerType::All);
        state.source = snapshot;
        state.mirror = display;
        state.pending = false;
        if (!accepted && previous) {
            const SourceWidgetSelection selection{SourceWidgetText::ClampOffset(snapshot, previous->anchor),
                                                  SourceWidgetText::ClampOffset(snapshot, previous->caret)};
            static_cast<void>(Select(selection));
        }
    }

    /** @copydoc SourceEditorWidget::Draw */
    std::optional<SourceEditingCommand> SourceEditorWidget::Draw(const SourceWidgetFrame &frame) {
        auto &state = *storage_;
        state.ownsInput = false;
        if (!state.editor || !ValidFrame(frame))
            return std::nullopt;
        ImGui::PushID(state.inputName.c_str());
        const bool visible = ImGui::BeginChild("surface", {frame.width, frame.height}, ImGuiChildFlags_None,
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        std::optional<SourceEditingCommand> intent;
        const bool inputEnabled = frame.inputEnabled && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) &&
                                  !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (visible) {
            const bool hovered = ImGui::IsWindowHovered();
            const bool focused = state.AcquireFocus(inputEnabled, frame.requestFocus, hovered);
            const auto origin = ImGui::GetCursorScreenPos();
            const auto extent = ImGui::GetContentRegionAvail();
            auto &display = state.PrepareDisplay(frame, origin, extent);
            state.RestoreViewport();
            state.Input(focused, hovered);
            state.ApplyScroll(inputEnabled && hovered, origin, extent);
            display.cursorColor = state.editor->GetTheme().GetColor(Zep::ThemeColor::CursorInsert);
            state.editor->Display();
            state.Ime(display, focused, origin);
            if (auto edit = state.CaptureEdit())
                intent = std::move(*edit);
            else if (state.planned)
                intent = std::move(*state.planned);
            else if (state.history)
                intent = *state.history;
            state.history.reset();
            state.planned.reset();
            // Native navigation/cut/paste may use transient commands. History belongs only to SourceDocumentService.
            state.buffer->GetUndoStack() = {};
            state.buffer->GetRedoStack() = {};
        }
        ImGui::EndChild();
        ImGui::PopID();
        return intent;
    }

    /** @copydoc SourceEditorWidget::Selection */
    SourceWidgetSelection SourceEditorWidget::Selection() const {
        return storage_->CaptureSelection();
    }

    /** @copydoc SourceEditorWidget::Identity */
    SourceEditingViewId SourceEditorWidget::Identity() const noexcept {
        return storage_->viewIdentity;
    }

    /** @copydoc SourceEditorWidget::OwnsInput */
    bool SourceEditorWidget::OwnsInput() const noexcept {
        return storage_->ownsInput;
    }

    /** @copydoc SourceEditorWidget::View */
    SourceEditingView SourceEditorWidget::View() const {
        const auto selected = Selection();
        return {selected.anchor, selected.caret, storage_->ScrollPosition(), storage_->viewIdentity};
    }

    /** @copydoc SourceEditorWidget::Select */
    bool SourceEditorWidget::Select(const SourceWidgetSelection selection) {
        auto &state = *storage_;
        if (!state.buffer || !Boundary(state.source, selection.anchor) || !Boundary(state.source, selection.caret))
            return false;
        const auto anchor = Zep::GlyphIterator(state.buffer, DisplayOffset(state.source, selection.anchor));
        const auto caret = Zep::GlyphIterator(state.buffer, DisplayOffset(state.source, selection.caret));
        static_cast<WidgetMode *>(state.buffer->GetMode())->Select(*state.buffer, anchor, caret);
        return true;
    }

    /** @copydoc SourceEditorWidget::Restore */
    bool SourceEditorWidget::Restore(const SourceEditingPublication &publication) {
        auto &state = *storage_;
        if (!state.buffer || !publication.restoration || publication.restoration->viewId != state.viewIdentity ||
            publication.snapshot.Revision() != state.source.Revision() || state.restoredRevision == publication.snapshot.Revision())
            return false;
        const auto &restoration = *publication.restoration;
        if (publication.snapshot.Identity().instance != state.source.Identity().instance || !std::isfinite(restoration.scrollLines) ||
            !Select({restoration.anchor, restoration.caret}))
            return false;
        state.pendingViewport = restoration.scrollLines;
        state.scroll = 0;
        state.restoredRevision = publication.snapshot.Revision();
        return true;
    }

    /** @copydoc SourceEditorWidget::Scroll */
    void SourceEditorWidget::Scroll(const float lines) {
        if (std::isfinite(lines) && std::isfinite(storage_->scroll + lines))
            storage_->scroll += lines;
    }

    /** @copydoc SourceEditorWidget::Decorate */
    bool SourceEditorWidget::Decorate(const std::uint64_t revision, const std::span<const SourceWidgetDecoration> decorations) {
        auto &state = *storage_;
        if (!state.buffer || revision != state.source.Revision() || decorations.size() > 4096)
            return false;
        for (const auto &decoration : decorations) {
            if (!ValidDecoration(state.source, decoration))
                return false;
        }
        std::vector<std::shared_ptr<Zep::RangeMarker>> markers;
        markers.reserve(decorations.size());
        for (const auto &decoration : decorations) {
            auto marker = std::make_shared<Zep::RangeMarker>(*state.buffer);
            marker->SetRange({static_cast<long>(DisplayOffset(state.source, decoration.begin)),
                              static_cast<long>(DisplayOffset(state.source, decoration.end))});
            marker->SetDescription(decoration.description);
            marker->displayType = Zep::RangeMarkerDisplayType::Underline | Zep::RangeMarkerDisplayType::Tooltip;
            marker->SetHighlightColor(DecorationColor(decoration.kind));
            markers.push_back(std::move(marker));
        }
        state.buffer->ClearRangeMarkers(Zep::RangeMarkerType::All);
        for (auto &marker : markers)
            state.buffer->AddRangeMarker(std::move(marker));
        return true;
    }
}  // namespace Horo::Editor
