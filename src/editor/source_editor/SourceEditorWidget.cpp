#include "SourceEditorWidget.h"

#include "Horo/Editor/EditorSettingsStore.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "SourceSyntax.h"
#include "SourceSyntaxProjection.h"
#include "SourceWidgetPlatform.h"
#include "SourceWidgetText.h"
#include "editor/input/EditorInputActions.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <format>
#include <imgui_internal.h>
#include <string_view>
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

    /** @brief One editor per view; Zep owns its components, callbacks, display and denied filesystem. */
    struct SourceEditorWidget::Storage final {
        SourceDocumentSnapshot source;
        std::unique_ptr<Zep::ZepEditor> editor;
        Zep::ZepBuffer *buffer{};
        Zep::ZepWindow *window{};
        std::string mirror;
        std::uint64_t update{};
        SourceSyntaxPresentation syntax;
        bool pending{};
        bool pointerCaptured{};
        Zep::GlyphIterator pointerAnchor;
        int fontZoom{};
        int baseFontPixels{14};
        std::uint64_t restoredRevision{};
        ImGuiID inputId{};
        float scroll{};
        std::optional<float> pendingViewport;
        bool ownsInput{};
        SourceEditingView beforeView;
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
        bool Matches(const SourceDocumentSnapshot &next, const std::string_view display) const {
            return editor && source.Revision() && source.Identity().instance == next.Identity().instance &&
                   display == buffer->GetBufferText(buffer->Begin(), buffer->End());
        }

        /** @brief Rebuilds only for a new identity or an authoritative replacement/rejection. */
        void Reset(const std::string &text) {
            auto candidate = MakeSourceWidgetEditor();
            candidate->RegisterGlobalMode(std::make_shared<WidgetMode>(*candidate));
            // Create the initial window before switching modes: Zep transfers the previous mode's bound window.
            // A fixed extension-free name never chooses a language service or syntax provider.
            auto *nextBuffer = candidate->InitWithText("source", text);
            candidate->SetGlobalMode(Zep::ZepMode_Standard::StaticName());
            nextBuffer->SetSyntax({});
            auto *nextWindow = candidate->GetActiveTabWindow()->GetActiveWindow();
            nextWindow->SetWindowFlags(nextWindow->GetWindowFlags() & ~Zep::WindowFlags::ShowWhiteSpace);
            editor = std::move(candidate);
            buffer = nextBuffer;
            window = nextWindow;
            pointerCaptured = false;
            pointerAnchor = window->GetBufferCursor();
            syntax.Reset();
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
        void Navigation(const std::uint32_t modifiers) const {
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
                if (const char *bytes = ImGui::GetClipboardText(); bytes && AdmittedText(bytes))
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

        /** @brief Resolves configured clipboard and selection actions through the shared registry. */
        bool Shortcuts(const std::uint32_t modifiers, const Input::InputBindingProfile &profile) {
            constexpr std::array commands{std::pair{kActionSourceSelectAll, ImGuiKey_A}, std::pair{kActionSourceCopy, ImGuiKey_C},
                                          std::pair{kActionSourcePaste, ImGuiKey_V}, std::pair{kActionSourceCut, ImGuiKey_X}};
            for (const auto &[action, key] : commands) {
                if (!Ui::ShortcutPressed(EditorActionBindings(action, profile)))
                    continue;
                if (key == ImGuiKey_A)
                    buffer->GetMode()->AddKeyPress('a', Zep::ModifierKey::Ctrl | modifiers);
                else
                    Clipboard(key);
                ImGui::GetIO().InputQueueCharacters.resize(0);
                return true;
            }
            return false;
        }

        /** @brief Inserts to the next four-column stop at a caret; selections and Shift-Tab use line indentation. */
        Result<SourceEditingPlan> TabPlan(const SourceEditingView &view, const bool outdent) const {
            constexpr unsigned indentationWidth = 4;
            if (outdent || view.anchor != view.caret)
                return PlanSourceIndentation(source, view, indentationWidth, outdent);
            const auto text = source.Text();
            const auto before = text.substr(0, view.caret);
            const auto newline = before.find_last_of("\r\n");
            const std::size_t firstLineStart = source.Metadata().utf8Bom ? 3 : 0;
            std::size_t start = newline == std::string_view::npos ? firstLineStart : newline + 1;
            std::size_t column = 0;
            for (; start < view.caret; ++start) {
                if (text[start] == '\t')
                    column += indentationWidth - column % indentationWidth;
                else if ((static_cast<std::byte>(text[start]) & std::byte{0xC0}) != std::byte{0x80})
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
                    if (auto plan = PlanSourceLanguageNewline(source, view); plan.HasValue())
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
        void Keyboard(const Input::InputBindingProfile &profile) {
            const auto &io = ImGui::GetIO();
            const bool command = io.KeyCtrl;
            std::uint32_t modifiers = io.KeyShift ? Zep::ModifierKey::Shift : 0;
            if (command)
                modifiers |= Zep::ModifierKey::Ctrl;
            if (io.KeyAlt)
                modifiers |= Zep::ModifierKey::Alt;
            if (Shortcuts(modifiers, profile))
                return;
            Navigation(modifiers);
            if (!command)
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
            const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
            if (clicked || pointerCaptured) {
                const auto previousSelection = CaptureSelection();
                // Display has resolved the current pointer against the current glyph layout.
                editor->OnMouseDown(mouse, Zep::ZepMouseButton::Left);
                const auto caret = window->GetBufferCursor();
                if (clicked) {
                    pointerAnchor = caret;
                    if (ImGui::GetIO().KeyShift) {
                        pointerAnchor = Zep::GlyphIterator(buffer, DisplayOffset(source, previousSelection.anchor));
                    }
                    pointerCaptured = true;
                }
                static_cast<WidgetMode *>(buffer->GetMode())->Select(*buffer, pointerAnchor, caret);
            }
            if (pointerCaptured && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                editor->OnMouseUp(mouse, Zep::ZepMouseButton::Left);
                pointerCaptured = false;
            }
        }

        /** @brief Keeps font-cache invalidation bounded to actual font or DPI changes. */
        WidgetDisplay &PrepareDisplay(const SourceWidgetFrame &frame, const ImVec2 origin, const ImVec2 extent, const Theme::Fonts *fonts) {
            auto &display = static_cast<WidgetDisplay &>(editor->GetDisplay());
            const float viewport = ScrollPosition();
            const auto generation = display.FontGeneration();
            const bool scaleChanged = display.GetPixelScale().x != frame.dpiScale || display.GetPixelScale().y != frame.dpiScale;
            if (scaleChanged)
                display.SetPixelScale({frame.dpiScale, frame.dpiScale});
            auto theme = ResolveSourceEditorTheme(Theme::GetActiveTokens());
            const float base = std::clamp(frame.fontSizePx > 0 ? frame.fontSizePx : theme.fontPixels / frame.dpiScale, 14.0F, 32.0F);
            baseFontPixels = static_cast<int>(std::lround(base));
            fontZoom = std::clamp(fontZoom, 14 - baseFontPixels, 32 - baseFontPixels);
            theme.fontPixels = std::clamp(base + static_cast<float>(fontZoom), 14.0F, 32.0F) * frame.dpiScale;
            const CodeEditorSettings defaults;
            const auto &preferences = frame.preferences ? *frame.preferences : defaults;
            auto flags = window->GetWindowFlags();
            const auto setFlag = [&](const auto flag, const bool enabled) {
                if (enabled)
                    flags |= flag;
                else
                    flags &= ~flag;
            };
            setFlag(Zep::WindowFlags::ShowWhiteSpace, preferences.showWhitespace);
            setFlag(Zep::WindowFlags::ShowLineNumbers, preferences.showLineNumbers);
            setFlag(Zep::WindowFlags::WrapText, preferences.wordWrap);
            setFlag(Zep::WindowFlags::HideScrollBar, !preferences.showScrollBar);
            if (flags != window->GetWindowFlags()) {
                window->SetWindowFlags(flags);
                window->DirtyLayout();
            }
            display.Configure(theme, fonts, frame.fontFamily);
            for (int type = 0; type < static_cast<int>(Zep::ZepTextType::Count); ++type)
                static_cast<void>(display.GetFont(static_cast<Zep::ZepTextType>(type)));
            if (scaleChanged || generation != display.FontGeneration())
                window->DirtyLayout();
            editor->SetDisplayRegion({origin.x, origin.y}, {origin.x + extent.x, origin.y + extent.y});
            if (scaleChanged || generation != display.FontGeneration())
                window->SetHoroViewportLines(viewport);
            UpdateSourceWidgetTheme(*editor, theme);
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
        bool HasTextInput(const Input::InputBindingProfile &profile) const {
            const auto &io = ImGui::GetIO();
            if (io.InputQueueCharacters.Size > 0 || ImGui::IsKeyPressed(ImGuiKey_Backspace) || ImGui::IsKeyPressed(ImGuiKey_Delete) ||
                ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Tab))
                return true;
            return Ui::ShortcutPressed(EditorActionBindings(kActionSourceCut, profile)) ||
                   Ui::ShortcutPressed(EditorActionBindings(kActionSourcePaste, profile));
        }

        /** @brief Allows this surface's pointer/wheel hit test past its own active text ID, preserving other input owners. */
        bool Hovered() const {
            const bool ownActiveItem = ImGui::GetActiveID() == ImGui::GetCurrentWindow()->GetID("text-input");
            return ImGui::IsWindowHovered(ownActiveItem ? ImGuiHoveredFlags_AllowWhenBlockedByActiveItem : ImGuiHoveredFlags_None);
        }

        /** @brief Acquires keyboard focus only from host handoff or a click in this enabled surface. */
        bool AcquireFocus(const bool enabled, const bool request, const bool hovered) {
            auto *guiWindow = ImGui::GetCurrentWindow();
            inputId = guiWindow->GetID("text-input");
            if (!hovered && !pointerCaptured && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                if (ImGui::GetActiveID() == inputId)
                    ImGui::ClearActiveID();
                return false;
            }
            if (enabled && (request || (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)))) {
                ImGui::FocusWindow(guiWindow);
                ImGui::SetActiveID(inputId, guiWindow);
            }
            return enabled && ImGui::IsWindowFocused() && (ImGui::GetActiveID() == 0 || ImGui::GetActiveID() == inputId);
        }

        /** @brief Captures only focused text/navigation input and claims the owning host's text-input channel. */
        void Input(const bool focused, const bool hovered, const Input::InputBindingProfile &profile, const bool skipKeyboard) {
            ownsInput = focused;
            if (focused) {
                ImGui::SetActiveID(inputId, ImGui::GetCurrentWindow());
                ImGui::KeepAliveID(inputId);
                // Native text controls allow pointer handoff while no selection gesture is held.
                ImGui::GetCurrentContext()->ActiveIdAllowOverlap = !ImGui::GetIO().MouseDown[ImGuiMouseButton_Left];
                ImGui::GetCurrentContext()->WantTextInputNextFrame = 1;
                ImGui::SetNextFrameWantCaptureKeyboard(true);
                if (HasTextInput(profile)) {
                    const auto selected = CaptureSelection();
                    beforeView = {selected.anchor, selected.caret, ScrollPosition(), viewIdentity};
                }
                if (!skipKeyboard)
                    Keyboard(profile);
            }
            if (focused && ((hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) ||
                            (pointerCaptured && ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                             (ImGui::GetIO().MouseDelta.x != 0 || ImGui::GetIO().MouseDelta.y != 0))))
                editor->ResetCursorTimer();
            if (hovered || pointerCaptured)
                editor->OnMouseMove(Zep::toNVec2f(ImGui::GetIO().MousePos));
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
            if (pendingViewport.has_value()) {
                window->SetHoroViewportLines(*pendingViewport);
                pendingViewport.reset();
            }
        }

        /** @brief Anchors platform IME data to this focused surface and releases stale active input on handoff. */
        void Ime(const WidgetDisplay &display, const bool focused, const ImVec2 origin) const {
            if (focused) {
                auto &ime = ImGui::GetCurrentContext()->PlatformImeData;
                ime.WantVisible = true;
                ime.InputPos = display.Cursor().has_value() ? Zep::toImVec2(display.Cursor()->topLeftPx) : origin;
                ime.InputLineHeight = std::round(Theme::GetActiveTokens().typography.body);
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
        storage_->inputName = std::format("source-widget-{}", viewIdentity.value);
    }

    SourceEditorWidget::~SourceEditorWidget() = default;

    /** @copydoc SourceEditorWidget::Publish */
    void SourceEditorWidget::Publish(const SourceDocumentSnapshot &snapshot) {
        auto &state = *storage_;
        if (snapshot.Revision() == 0) {
            const auto identity = state.viewIdentity;
            const auto &name = state.inputName;
            auto replacement = std::make_unique<Storage>();
            replacement->viewIdentity = identity;
            replacement->inputName = name;
            storage_ = std::move(replacement);
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
    std::optional<SourceEditingCommand> SourceEditorWidget::Draw(const SourceWidgetFrame &frame, const Theme::Fonts *fonts) {
        auto &state = *storage_;
        state.ownsInput = false;
        if (!state.editor || !ValidSourceWidgetFrame(frame))
            return std::nullopt;
        ImGui::PushID(state.inputName.c_str());
        const bool visible = ImGui::BeginChild("surface", {frame.width, frame.height}, ImGuiChildFlags_None,
                                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        std::optional<SourceEditingCommand> intent;
        const bool inputEnabled = frame.inputEnabled && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled) &&
                                  !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (visible) {
            const bool hovered = state.Hovered();
            const bool focused = state.AcquireFocus(inputEnabled, frame.requestFocus, hovered);
            const auto origin = ImGui::GetCursorScreenPos();
            const auto extent = ImGui::GetContentRegionAvail();
            auto &display = state.PrepareDisplay(frame, origin, extent, fonts);
            state.RestoreViewport();
            const Input::InputBindingProfile defaultKeymap;
            state.Input(focused, hovered, frame.keymap ? *frame.keymap : defaultKeymap, frame.skipKeyboard);
            state.ApplyScroll(inputEnabled && hovered, origin, extent);
            display.SetCursorColor(state.editor->GetTheme().GetColor(Zep::ThemeColor::CursorInsert));
            display.Bind(state.buffer, state.syntax.ForMirror(state.buffer->GetUpdateCount()));
            state.editor->Display();
            state.Pointer(focused, hovered);
            display.Bind(nullptr, nullptr);
            state.Ime(display, focused, origin);
            if (auto edit = state.CaptureEdit())
                intent = std::move(*edit);
            else if (state.planned)
                intent = std::move(*state.planned);
            state.planned.reset();
            // Native navigation/cut/paste may use transient commands. History belongs only to SourceDocumentService.
            state.buffer->GetUndoStack() = {};
            state.buffer->GetRedoStack() = {};
        }
        ImGui::EndChild();
        ImGui::PopID();
        return intent;
    }

    /** @copydoc SourceEditorWidget::Highlight */
    bool SourceEditorWidget::Highlight(const std::uint64_t revision, const SourceSyntax &syntax) {
        auto &state = *storage_;
        if (!state.buffer || state.pending || revision != state.source.Revision())
            return false;
        return state.syntax.Publish(state.source, state.buffer->GetUpdateCount(), syntax);
    }

    /** @copydoc SourceEditorWidget::HasHighlight */
    bool SourceEditorWidget::HasHighlight(const std::uint64_t revision) const noexcept {
        const auto &state = *storage_;
        return state.buffer && !state.pending && state.source.Revision() == revision &&
               state.syntax.Contains(revision, state.buffer->GetUpdateCount());
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

    /** @copydoc SourceEditorWidget::SetFontZoom */
    void SourceEditorWidget::SetFontZoom(const int steps) {
        storage_->fontZoom = std::clamp(steps, 14 - storage_->baseFontPixels, 32 - storage_->baseFontPixels);
    }

    /** @copydoc SourceEditorWidget::FontZoom */
    int SourceEditorWidget::FontZoom() const noexcept {
        return storage_->fontZoom;
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
        return PublishSourceWidgetDecorations(*state.buffer, state.source, decorations);
    }
}  // namespace Horo::Editor
