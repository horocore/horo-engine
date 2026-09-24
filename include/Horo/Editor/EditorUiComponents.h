/**
 * @file EditorUiComponents.h
 * @brief Typed primitive and composite contracts for the editor design system.
 */
#pragma once

#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"

#include <array>
#include <cstdint>
#include <functional>
#include <imgui.h>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor::Ui {

    // ── Semantic button variant ──────────────────────────────────────────

    /** @brief Semantic button variant for shared editor buttons. */
    enum class ButtonVariant {
        Primary,
        Secondary,
    };

    using ComponentSize = DesignSystem::ComponentSize;
    using SpacingSize = DesignSystem::SpacingSize;

    /** @brief Horizontal sizing behavior shared by primitive style properties. */
    enum class StyleWidth {
        FitContent,
        FillAvailable,
    };

    /** @brief Optional call-site presentation shared by editor primitives. */
    struct StyleProperties {
        StyleWidth width{StyleWidth::FitContent}; /**< Horizontal sizing behavior. */
        std::optional<SpacingSize> paddingX{};    /**< Horizontal token override; empty inherits the size preset. */
        std::optional<SpacingSize> paddingY{};    /**< Vertical token override; empty inherits the size preset. */
    };

    /**
     * @brief Resolves one logical layout dimension through the active global UI scale.
     * @param value Unscaled logical UI value.
     * @return Display-scaled value.
     */
    [[nodiscard]] float ScaledLayoutValue(float value) noexcept;

    /** @brief RAII scope for theme-backed tooltip chrome and typography. */
    class ScopedTooltip {
    public:
        /**
         * @brief Begins a tooltip using shared padding, surface, border, radius, and label typography.
         * @param fonts Optional editor font set; null preserves the current ImGui font.
         */
        explicit ScopedTooltip(const Theme::Fonts *fonts = nullptr);

        /** @brief Ends the tooltip and restores the previous ImGui style. */
        ~ScopedTooltip();

        ScopedTooltip(const ScopedTooltip &) = delete;
        ScopedTooltip &operator=(const ScopedTooltip &) = delete;

    private:
        ImFont *font_{nullptr};
        float fontScale_{1.0F};
    };

    /**
     * @brief Draws a single theme-backed, width-constrained tooltip.
     * @param text Visible tooltip text.
     * @param fonts Optional editor font set; null preserves the current ImGui font.
     */
    void ShowTooltip(const char *text, const Theme::Fonts *fonts = nullptr);

    // ── Button props & primitive ─────────────────────────────────────────

    /** @brief Input contract for the shared editor button primitive. */
    struct ButtonProps {
        const char *label = "";                              /**< Visible label plus stable ImGui identity suffix. */
        ImVec2 size = {0.0F, 0.0F};                          /**< Optional logical size; zero dimensions use theme metrics. */
        ButtonVariant variant = ButtonVariant::Primary;      /**< Semantic color treatment. */
        bool enabled = true;                                 /**< Whether the action can be activated. */
        ImFont *font = nullptr;                              /**< Optional font atlas entry. */
        float baseFontSize = Theme::FontPx::Sans;            /**< Atlas size used to calculate the font scale. */
        ComponentSize componentSize = ComponentSize::Medium; /**< Shared theme-backed geometry preset. */
        StyleProperties style{};                             /**< Optional call-site presentation overrides. */
    };

    /**
     * @brief Draws a shared editor button primitive.
     * @param props Stable identity, state, semantic variant, sizing, and optional style overrides.
     * @return True when the enabled button was activated this frame.
     */
    [[nodiscard]] bool Button(const ButtonProps &props);

    // ── Configurable data table primitive ────────────────────────────────

    /** @brief One visible/configurable column in a shared data table. */
    struct TableColumn {
        std::string id;
        std::string label;
        float width{0.0F};
        bool visible{true};
    };

    /** @brief One selectable cell in a shared data table row. */
    struct TableCell {
        std::string text;
        ImVec4 color{Theme::Text()};
    };

    /** @brief Row data consumed by the shared data table primitive. */
    struct TableRow {
        std::vector<TableCell> cells;
    };

    /** @brief Rendering options for a shared data table. */
    struct TableProps {
        const char *id{"##Table"};
        ComponentSize componentSize{ComponentSize::Small};
        bool selectableCells{true};
    };

    /** @brief Optional activation produced by one selectable table cell. */
    struct TableInteraction {
        std::optional<std::size_t> activatedRow;
        std::optional<std::size_t> activatedColumn;
    };

    /** @brief Draws a themed table with runtime column visibility and selectable cells. */
    [[nodiscard]] TableInteraction DrawTable(const TableProps &props, std::span<const TableColumn> columns, std::span<const TableRow> rows,
                                             const Theme::Fonts &fonts);

    // ── Card / surface primitives ────────────────────────────────────────

    /** @brief RAII child surface matching the shared card visual contract. */
    class ScopedCard {
    public:
        explicit ScopedCard(const char *id, ImVec2 size, float padX = Theme::Layout::CardPad, float padY = Theme::Layout::CardPad,
                            ImVec4 bg = Theme::Bg2(), bool autoResizeY = false);
        ~ScopedCard();

        ScopedCard(const ScopedCard &) = delete;
        ScopedCard &operator=(const ScopedCard &) = delete;
    };

    // ── Icon helpers ─────────────────────────────────────────────────────

    /** @brief Draws an icon-only close button using vector strokes, not glyph text. */
    [[nodiscard]] bool IconCloseButton(const char *id, ImVec2 size);

    /** @brief Direction rendered by the compact navigation icon button. */
    enum class NavigationIcon : std::uint8_t {
        Back,
        Forward,
        Up,
    };

    /**
     * @brief Draws a borderless navigation button with a compact hover surface.
     * @param id Stable UI identity.
     * @param icon Directional icon to render.
     * @param size Exact interaction bounds.
     * @param enabled Whether the action may be invoked.
     * @return True when an enabled button was activated.
     */
    [[nodiscard]] bool NavigationIconButton(const char *id, NavigationIcon icon, ImVec2 size, bool enabled = true);

    /** @brief Vector glyph supported by the shared compact icon button. */
    enum class IconButtonGlyph : std::uint8_t {
        Plus,
        Folder,
    };

    /** @brief Input contract for a shared compact icon-only action. */
    struct IconButtonProps {
        const char *id = "";
        IconButtonGlyph glyph = IconButtonGlyph::Plus;
        ImVec2 size = {0.0F, 0.0F};
        const char *tooltip = "";
        bool enabled = true;
        ComponentSize componentSize = ComponentSize::Medium;
    };

    /**
     * @brief Draws a themed icon-only action with hover, focus, and tooltip states.
     * @param props Stable identity, glyph, geometry, tooltip, and enabled state.
     * @return True when the enabled action was activated.
     */
    [[nodiscard]] bool IconButton(const IconButtonProps &props);

    // ── Typography primitives ────────────────────────────────────────────

    /** @brief Draws an uppercase section label. */
    void SectionTitle(const char *upperCaseLabel, const Theme::Fonts &fonts);

    /** @brief Draws an uppercase field label. */
    void FieldLabel(const char *upperCaseLabel, const Theme::Fonts &fonts);

    /** @brief Renders a hint string wrapped under a field. */
    void Hint(const char *text, const Theme::Fonts &fonts);

    /** @brief Renders an error string wrapped under a field. */
    void ErrorText(const char *text, const Theme::Fonts &fonts);

    /**
     * @brief Draws compact clickable text without a button surface.
     * @param id Stable UI identity.
     * @param label Visible link text.
     * @param font Font used for measurement and rendering.
     * @param fontSize Visible text size.
     * @param current Whether the link represents the current destination.
     * @return True when activated.
     */
    [[nodiscard]] bool TextLink(const char *id, const char *label, ImFont *font, float fontSize, bool current = false);

    // ── Badge / tag primitives ───────────────────────────────────────────

    /** @brief Semantic visual tone for reusable badges and tags. */
    enum class BadgeTone : std::uint8_t {
        Neutral,
        Accent,
        Success,
        Warning,
        Error,
    };

    /** @brief Shared geometry presets for badges and status pills. */
    enum class BadgeSize : std::uint8_t {
        Small,
        Medium,
    };

    /** @brief Input contract for the shared compact badge/tag primitive. */
    struct BadgeProps {
        const char *label = "";              /**< Visible badge text. */
        BadgeTone tone = BadgeTone::Neutral; /**< Semantic color treatment. */
        BadgeSize size = BadgeSize::Small;   /**< Shared geometry preset. */
        bool leadingIndicator = false;       /**< Whether to draw a status dot before the label. */
    };

    /**
     * @brief Draws a compact, vertically centred badge and advances the inline cursor.
     * @param props Visible content and semantic presentation.
     * @param fonts Editor typography handles.
     */
    void Badge(const BadgeProps &props, const Theme::Fonts &fonts);

    /**
     * @brief Returns the exact horizontal space consumed by @ref Badge.
     * @param props Visible content and semantic presentation.
     * @param fonts Editor typography handles.
     * @return Width in logical UI pixels.
     */
    [[nodiscard]] float BadgeWidth(const BadgeProps &props, const Theme::Fonts &fonts);

    /**
     * @brief Resolves the semantic foreground color used by a badge tone.
     * @param tone Semantic badge state.
     * @return Theme-derived foreground color.
     */
    [[nodiscard]] ImVec4 BadgeToneColor(BadgeTone tone);

    // ── Separator ────────────────────────────────────────────────────────

    /** @brief Draws a dashed horizontal separator across available width. */
    void DashedSeparator(float dash = 4.0F, float gap = 3.0F);

    /**
     * @brief Draws a compact label followed by a horizontal separator.
     * @param label Visible separator label.
     * @param fonts Editor typography handles.
     */
    void LabeledSeparator(const char *label, const Theme::Fonts &fonts);

    // ── Settings / form row primitives ───────────────────────────────────

    /**
     * @brief Draws a settings section group heading with a horizontal rule.
     *
     * @param first Pass true for the first group in a section to suppress
     *              the top spacing.
     */
    void SettingGroup(const char *label, const Theme::Fonts &fonts, bool first = false);

    /**
     * @brief Generic two-column settings row.
     *
     * Column 0 holds the label + optional description; column 1
     * invokes the control callback and has a fixed width of
     * `Theme::Layout::ControlW`.
     */
    template <typename ControlFn> void SettingRow(const char *label, const char *description, const Theme::Fonts &f, ControlFn &&control) {
        ImGui::PushID(label);
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{0.0F, 0.0F});
        if (ImGui::BeginTable("row", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("info", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthFixed, Theme::Layout::ControlW);
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::BeginGroup();
            {
                Theme::ScopedTextStyle ts(f.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
                ImGui::TextUnformatted(label);
                ImGui::PopStyleColor();
            }
            if (description != nullptr && description[0] != '\0') {
                Theme::ScopedTextStyle ts(f.sans, Theme::TextPx::Caption(), Theme::FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 16.0F);
                ImGui::TextWrapped("%s", description);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();

            ImGui::TableSetColumnIndex(1);
            control();

            ImGui::EndTable();
        }
        ImGui::PopStyleVar();

        ImGui::Dummy({0.0F, 10.0F});
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine({p.x, p.y}, {p.x + w, p.y}, Theme::U32(Theme::Border()), 1.0F);
        ImGui::Dummy({0.0F, 10.0F});
        ImGui::PopID();
    }

    // ── Form controls ────────────────────────────────────────────────────

    /** @brief Non-owning callbacks used by a combo whose entries come from a typed model. */
    struct ComboItemSource {
        std::function<const char *(int index)> label;           /**< Required display-label projection. */
        std::function<bool(int index)> enabled;                 /**< Optional selection predicate; empty enables all. */
        std::function<const char *(int index)> disabledTooltip; /**< Optional diagnostic for disabled rows. */
    };

    /** @brief Semantic surface used by a shared combo control. */
    enum class ComboControlSurface {
        Default,
        BottomDockToolbar,
    };

    /** @brief Optional visual configuration shared by combo controls. */
    struct ComboControlOptions {
        bool error{false};
        float height{0.0F};
        ComponentSize componentSize{ComponentSize::Small};
        ComboControlSurface surface{ComboControlSurface::Default};
        UiIcon leadingIcon{UiIcon::None}; /**< Optional semantic icon before the selected value. */
    };

    /** @brief Renders a styled dropdown with optional error styling. Returns true if the selection changed. */
    [[nodiscard]] bool ComboControl(const char *id, int *value, const char *const *items, int itemCount, const Theme::Fonts &fonts,
                                    ComboControlOptions options = {});

    /**
     * @brief Renders the shared dropdown design for entries projected from a typed model.
     * @param id Stable UI identity.
     * @param value Selected entry index.
     * @param itemCount Number of entries exposed by @p source.
     * @param source Non-owning entry callbacks valid for the duration of this call.
     * @param fonts Editor typography handles.
     * @param options Optional error, height, and size presentation.
     * @return True when an enabled entry changed the selection.
     */
    [[nodiscard]] bool ComboControl(const char *id, int *value, int itemCount, const ComboItemSource &source, const Theme::Fonts &fonts,
                                    ComboControlOptions options = {});

    /** @brief Semantic surface used by a shared text input. */
    enum class InputTextSurface {
        Default,
        BottomDockToolbar,
    };

    /** @brief Optional visual configuration shared by text inputs. */
    struct InputTextOptions {
        bool error{false};                                   /**< Whether to render validation-error styling. */
        float width{-1.0F};                                  /**< Logical width; negative fills available content. */
        const char *hint{nullptr};                           /**< Optional placeholder text. */
        float prefixIconWidth{0.0F};                         /**< Left padding reserved for a caller-drawn icon. */
        ComponentSize componentSize{ComponentSize::Small};   /**< Shared theme-backed size preset. */
        InputTextSurface surface{InputTextSurface::Default}; /**< Semantic background surface. */
    };

    /**
     * @brief Renders a themed multi-select field with a checkbox popup.
     * @param id Stable UI identity.
     * @param label Visible summary label.
     * @param items Visible option labels.
     * @param values Mutable selection values paired with @p items.
     * @param fonts Editor typography handles.
     * @param width Logical field width; zero uses the label's natural width.
     * @param componentSize Shared theme-backed size preset.
     * @return True when any selection value changed.
     */
    [[nodiscard]] bool MultiSelectField(const char *id, const char *label, std::span<const char *const> items, std::span<bool> values,
                                        const Theme::Fonts &fonts, float width = 0.0F, ComponentSize componentSize = ComponentSize::Small);

    /**
     * @brief Renders an input text field with shared frame styling and optional error state.
     * @param id Stable UI identity.
     * @param buffer Mutable null-terminated text buffer.
     * @param bufferSize Capacity of @p buffer, including the null terminator.
     * @param fonts Editor typography handles.
     * @param options Optional validation and presentation configuration.
     * @return True when the text changed.
     */
    [[nodiscard]] bool InputTextControl(const char *id, char *buffer, size_t bufferSize, const Theme::Fonts &fonts,
                                        const InputTextOptions &options = {});

    /**
     * @brief Renders the string-backed overload of the shared input text field.
     * @param id Stable UI identity.
     * @param value Mutable text value.
     * @param maxSize Maximum storage size, including the null terminator.
     * @param fonts Editor typography handles.
     * @param options Optional validation and presentation configuration.
     * @return True when the text changed.
     */
    [[nodiscard]] bool InputTextControl(const char *id, std::string &value, size_t maxSize, const Theme::Fonts &fonts,
                                        const InputTextOptions &options = {});

    /**
     * @brief Presentation metadata for one line in a selectable text block.
     */
    struct SelectableTextLineLayout {
        ImVec4 color{};                          /**< Text color for the complete line. */
        std::size_t alignedColumnByteOffset{0U}; /**< Optional byte offset drawn at the block's shared aligned column. */
    };

    /**
     * @brief Renders a frameless read-only text block with native multi-line selection and copy support.
     * @param id Stable UI identity.
     * @param buffer Mutable null-terminated storage retained unchanged by the read-only control.
     * @param bufferSize Capacity of @p buffer, including the null terminator.
     * @param lineLayouts Presentation metadata for each newline-delimited line in @p buffer.
     * @param alignedColumnX Shared horizontal column offset used by non-zero line layout offsets.
     * @param width Requested interaction width; negative values fill the remaining content width.
     * @return True while the text selection surface owns keyboard focus.
     */
    [[nodiscard]] bool SelectableTextBlock(const char *id, char *buffer, size_t bufferSize,
                                           std::span<const SelectableTextLineLayout> lineLayouts, float alignedColumnX = 0.0F,
                                           float width = -1.0F);

    /**
     * @brief Draws a hex color input paired with a clickable swatch and anchored picker popup.
     *
     * @param id Stable UI identity.
     * @param buffer Mutable `#RRGGBB` draft buffer. Invalid intermediate text leaves the last valid swatch visible.
     * @param bufferSize Buffer capacity.
     * @param fonts Editor typography handles.
     * @return True when a valid color value was committed into @p buffer by typing or the picker.
     */
    [[nodiscard]] bool ColorHexControl(const char *id, char *buffer, size_t bufferSize, const Theme::Fonts &fonts);
    [[nodiscard]] bool ColorHexControl(const char *id, std::string &value, size_t maxSize, const Theme::Fonts &fonts);

    enum class SliderValueFormat : std::uint8_t {
        Integer,
        Minutes,
        Percent,
        Milliseconds,
    };

    /** @brief Integer input with shared frame styling. */
    void InputIntControl(const char *id, int *value, const Theme::Fonts &fonts);

    /** @brief Float input with shared frame styling. */
    void InputFloatControl(const char *id, float *value, const Theme::Fonts &fonts);

    /**
     * @brief Draws a single numeric field with compact in-field increment and decrement actions.
     * @param id Stable control identity.
     * @param value Edited value.
     * @param fonts Editor font handles.
     * @param step Amount applied by each arrow action.
     * @return True when typing or an arrow action changed the value.
     */
    [[nodiscard]] bool InputFloatStepperControl(const char *id, float *value, const Theme::Fonts &fonts, float step = 0.1F);

    /**
     * @brief Custom slider imitating an HTML <input type="range">.
     *
     * @param format Typed format for the value label.
     * @param step   Quantisation step; a value of 25 for a 75–200 scale snaps to 75/100/125/….
     */
    void SliderIntControl(const char *id, int *value, int minValue, int maxValue, SliderValueFormat format, const Theme::Fonts &fonts,
                          int step = 1);

    /**
     * @brief iOS-style toggle switch.
     *
     * @param showLabel If true, draws an "Enabled" / "Disabled" label to the right.
     * @return True when the toggle was clicked.
     */
    [[nodiscard]] bool ToggleControl(const char *id, bool *value, const Theme::Fonts &fonts, bool showLabel = true);

    /**
     * @brief A standard styled checkbox.
     *
     * @param label The label to show next to the checkbox.
     * @param value Pointer to the boolean state.
     * @param fonts The application font set.
     * @param minimumBoxSize Optional minimum square size in logical pixels.
     * @return True when the checkbox was clicked.
     */
    [[nodiscard]] bool CheckboxControl(const char *label, bool *value, const Theme::Fonts &fonts, float minimumBoxSize = 0.0F);

    // ── Higher-order helpers ─────────────────────────────────────────────

    /**
     * @brief A plugin row: version + description + toggle, rendered inside
     *        a SettingRow for the plugin name.
     */
    void PluginRow(const char *name, const char *version, const char *description, bool *enabled, const Theme::Fonts &fonts);

    /** @brief Draws a keyboard shortcut display (keycap chips). */
    void ShortcutDisplay(const char *a, const char *b, const char *c, const Theme::Fonts &fonts);

    /**
     * @brief Interactive shortcut key recorder.
     *
     * When @p listening is false, renders the current key binding as kbd chips
     * inside a dashed-border clickable area. Sets @p listening = true on click.
     *
     * When @p listening is true, shows "Press keys..." with a pulse effect
     * and polls ImGui key state. On next non-modifier key press, writes the
     * combo string into @p keysOut (truncated to @p keysOutSize) and sets
     * @p listening = false. Escape cancels without writing.
     *
     * @return true if a new key combo was just recorded this frame.
     */
    [[nodiscard]] bool ShortcutRecorder(const char *id, const char *keysLabel, bool *listening, std::string &keysOut,
                                        const Theme::Fonts &fonts, const char *placeholderText = "Click to record",
                                        const char *listeningText = "Press keys...");

    /** @brief Draws a colour-theme chip (swatch dot + label). Returns true when clicked. */
    [[nodiscard]] bool ThemeChip(const char *label, ImVec4 swatch, bool active, const Theme::Fonts &fonts);

    // ── Modal layout primitives ──────────────────────────────────────────

    /** @brief Optional screen-space region in which a modal is centered and dragged. */
    struct ModalPlacementRegion {
        ImVec2 position{};
        ImVec2 size{};
    };

    /** @brief Shared geometry and chrome configuration for an editor workflow modal. */
    struct ModalShellProps {
        const char *id = "EditorModal"; /**< Stable ImGui window identity. */
        const char *title = "";         /**< Visible title-bar text. */
        ImVec2 requestedSize = {Theme::Layout::ModalW, Theme::Layout::ModalH};
        float viewportPadding = 48.0F;
        float minimumWidth = 360.0F;
        float minimumHeight = 360.0F;
        float headerHeight = Theme::Layout::HeaderH;
        float footerHeight = Theme::Layout::FooterH;
        std::optional<ModalPlacementRegion> placementRegion; /**< Defaults to the main viewport's usable area. */
        ImTextureID logo = 0;
        bool showBrandMark = false;
        bool showClose = true;
        float titleFontSize = Theme::TextPx::Title();
        ComponentSize componentSize = ComponentSize::Medium;
    };

    /**
     * @brief RAII modal window with shared positioning, title bar, body geometry,
     *        footer surface, and close action.
     *
     * Exclusive focus and close policy remain owned by EditorModalHost. This
     * component owns presentation only.
     */
    class ScopedModalShell {
    public:
        /**
         * @brief Begins a modal window and draws its shared title-bar chrome.
         * @param props Modal identity, geometry, title, and optional brand treatment.
         * @param fonts Editor typography handles.
         */
        ScopedModalShell(const ModalShellProps &props, const Theme::Fonts &fonts);

        /** @brief Ends any open footer and releases the modal ImGui style stack. */
        ~ScopedModalShell();

        ScopedModalShell(const ScopedModalShell &) = delete;
        ScopedModalShell &operator=(const ScopedModalShell &) = delete;

        /** @brief Returns the vertical space between title bar and footer. */
        [[nodiscard]] float BodyHeight() const noexcept;

        /** @brief Returns the local Y coordinate where the footer begins. */
        [[nodiscard]] float FooterStartY() const noexcept;

        /** @brief Returns true when the title-bar close action was activated. */
        [[nodiscard]] bool CloseRequested() const noexcept;

        /**
         * @brief Begins the shared fixed footer surface.
         * @param padding Inner footer padding.
         * @param border Whether the footer child owns a border.
         */
        void BeginFooter(ImVec2 padding, bool border = false);

        /** @brief Ends the footer surface opened by @ref BeginFooter. */
        void EndFooter();

    private:
        float bodyHeight_{};
        float footerStartY_{};
        float footerHeight_{};
        bool closeRequested_{};
        bool footerOpen_{};
    };

    /** @brief Presentation configuration for a modal sidebar/content split. */
    struct ModalSplitPaneProps {
        const char *id = "ModalSplitPane";
        ImVec2 size = {0.0F, 0.0F};
        float leadingWidth = Theme::Layout::SidebarW;
        float gap = 0.0F;
        ImVec2 leadingPadding = {Theme::Layout::SidebarPadX, Theme::Layout::SidebarPadY};
        ImVec2 contentPadding = {Theme::Layout::BodyPadX, Theme::Layout::BodyPadY};
        ImVec4 leadingBackground = Theme::Bg0();
        ImVec4 contentBackground = Theme::Bg1();
        bool drawDivider = true;
        bool leadingScrollable = false;
        bool contentScrollable = true;
    };

    /**
     * @brief Draws an optional-navigation modal layout without retaining callbacks.
     * @param props Split geometry and semantic surfaces.
     * @param drawLeading Draws sidebar/navigation content.
     * @param drawContent Draws the primary pane content.
     */
    template <typename LeadingFn, typename ContentFn>
    void ModalSplitPane(const ModalSplitPaneProps &props, LeadingFn &&drawLeading, ContentFn &&drawContent) {
        ImGui::PushID(props.id);
        const ImGuiWindowFlags leadingFlags =
            props.leadingScrollable
                ? ImGuiWindowFlags_AlwaysUseWindowPadding
                : ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        const ImGuiWindowFlags contentFlags =
            props.contentScrollable
                ? ImGuiWindowFlags_AlwaysUseWindowPadding
                : ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, props.leadingPadding);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, props.leadingBackground);
        ImGui::BeginChild("##leading", {props.leadingWidth, props.size.y}, false, leadingFlags);
        drawLeading();
        const ImVec2 leadingPosition = ImGui::GetWindowPos();
        const ImVec2 leadingSize = ImGui::GetWindowSize();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        if (props.drawDivider) {
            ImGui::GetWindowDrawList()->AddLine({leadingPosition.x + leadingSize.x - 1.0F, leadingPosition.y},
                                                {leadingPosition.x + leadingSize.x - 1.0F, leadingPosition.y + leadingSize.y},
                                                Theme::U32(Theme::Border()), 1.0F);
        }

        ImGui::SameLine(0.0F, props.gap);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, props.contentPadding);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, props.contentBackground);
        ImGui::BeginChild("##content", {0.0F, props.size.y}, false, contentFlags);
        drawContent();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    // ── Dock UI ───────────────────────────────────────────────────────────

    /** @brief Typography and border treatment for a dock tab strip. */
    enum class DockTabStyle : std::uint8_t {
        Default,
        GlobalDock,
    };

    /**
     * @brief Draws a horizontal editor dock tab strip.
     * @param tabs Localized visible tab labels in display order.
     * @param activeTab Zero-based active tab index.
     * @param fonts Editor font handles valid for this component call.
     * @param height Exact strip height.
     * @param style Semantic visual role for typography and border tokens.
     * @return The activated tab index, or @p activeTab when no tab was pressed.
     */
    int DrawDockTabs(std::span<const char *const> tabs, int activeTab, const Theme::Fonts &fonts, float height = 26.0F,
                     DockTabStyle style = DockTabStyle::Default);

    /**
     * @brief Draws the compact 36-pixel tab strip used by left and right workspace docks.
     * @param tabs Localized visible tab labels in display order.
     * @param activeTab Zero-based active tab index.
     * @param fonts Editor font handles valid for this component call.
     * @return The activated tab index, or @p activeTab when no tab was pressed.
     */
    int DrawSideDockTabs(std::span<const char *const> tabs, int activeTab, const Theme::Fonts &fonts);

    void DrawObjTitle(const char *title, const char *badgeText, ImVec4 badgeBg, ImVec4 badgeFg, const Theme::Fonts &fonts);

    /** @brief Interaction result returned by a shared editable text surface. */
    struct TextEditResult {
        bool changed{false};   /**< Text changed during the current frame. */
        bool committed{false}; /**< Enter or focus loss completed the interaction. */
        bool cancelled{false}; /**< Escape requested restoration of the caller-owned draft. */
        bool active{false};    /**< The text widget owns keyboard focus after this frame. */
    };

    /** @brief Layout and presentation for an editable title with caller-owned trailing content. */
    struct EditableTitleProps {
        UiIcon leadingIcon{UiIcon::Generic}; /**< Semantic icon drawn before the title field. */
        float trailingWidth{0.0F};           /**< Width reserved for trailing controls drawn by the caller. */
        bool error{false};                   /**< Whether to render the title field in its validation-error state. */
    };

    /**
     * @brief Draws an editable title field with a semantic leading icon.
     * @param id Stable UI identity scoped by the caller.
     * @param value Mutable caller-owned title draft.
     * @param maximumBytes Maximum UTF-8 content bytes, excluding the null terminator.
     * @param fonts Editor typography handles.
     * @param props Leading icon, trailing reservation and validation presentation.
     * @return Per-frame text interaction state.
     */
    [[nodiscard]] TextEditResult DrawEditableTitle(const char *id, std::string &value, size_t maximumBytes, const Theme::Fonts &fonts,
                                                   const EditableTitleProps &props = {});

    /** @brief One localized action rendered inside a card action menu. */
    struct CardMenuAction {
        const char *id{""};             /**< Stable ImGui identity. */
        const char *label{""};          /**< Caller-localized visible label. */
        UiIcon icon{UiIcon::None};      /**< Optional semantic icon. */
        bool enabled{true};             /**< Whether the action accepts input. */
        bool destructive{false};        /**< Whether to use the destructive text treatment. */
        bool separatorBefore{false};    /**< Whether a separator precedes this action. */
        std::function<void()> onInvoke; /**< Frame-local callback invoked on activation. */
    };

    /** @brief One icon action rendered at the trailing edge of a card title bar. */
    struct CardTitleBarAction {
        const char *id{""};                        /**< Stable ImGui identity. */
        UiIcon icon{UiIcon::None};                 /**< Semantic action icon. */
        const char *title{""};                     /**< Caller-localized tooltip text. */
        bool enabled{true};                        /**< Whether the action accepts input. */
        bool active{false};                        /**< Whether to use the active accent treatment. */
        std::function<void()> onInvoke;            /**< Direct action callback when no menu is supplied. */
        std::span<const CardMenuAction> menuItems; /**< Optional popup actions owned by the caller for this frame. */
    };

    /** @brief Presentation and actions for a generic card title bar. */
    struct CardTitleBarProps {
        const char *id{""};                          /**< Stable title-bar identity scoped by the card. */
        const char *title{""};                       /**< Caller-localized visible title. */
        const Theme::Fonts &fonts;                   /**< Editor font handles valid for this component call. */
        std::span<const CardTitleBarAction> actions; /**< Ordered trailing actions. */
    };

    /** @brief Construction properties for a generic panel card. */
    struct CardProps {
        const char *id{""};         /**< Stable card identity scoped by the caller. */
        bool defaultExpanded{true}; /**< Disclosure state used before this card has stored UI state. */
    };

    /** @brief RAII panel card with an optional action-bearing title bar and auto-height body. */
    class Card final {
    public:
        /** @brief Begins an auto-height card. @param props Stable card construction properties. */
        explicit Card(const CardProps &props);
        /** @brief Ends the card when its scope exits. */
        ~Card();

        Card(const Card &) = delete;
        Card &operator=(const Card &) = delete;
        Card(Card &&) = delete;
        Card &operator=(Card &&) = delete;

        /** @brief Draws the card title bar and invokes activated frame-local actions. */
        void DrawTitleBar(const CardTitleBarProps &props);
        /** @brief Begins the padded card body when expanded. @return True when callers should render body content. */
        [[nodiscard]] bool BeginBody();
        /** @brief Ends the card once; later calls are ignored. */
        void Finish();

    private:
        bool open_{true};
        bool bodyOpen_{false};
        bool expanded_{true};
        ImGuiStorage *stateStorage_{nullptr};
        ImGuiID expandedStateId_{};
    };

    void DrawPropRow(const char *label, const char *value, const Theme::Fonts &fonts);

    /** @brief Semantic text tone for shared context-menu actions. */
    enum class ContextMenuItemTone {
        Normal,
        Danger,
    };

    /**
     * @brief Opens a styled context popup for the preceding ImGui item when requested.
     * @param id Stable popup identity scoped by the caller.
     * @return True while the popup is open; pair with @ref EndContextMenu.
     */
    [[nodiscard]] bool BeginContextMenu(const char *id);

    /** @brief Opens a styled context popup for empty space in the current window. */
    [[nodiscard]] bool BeginContextWindowMenu(const char *id);

    /** @brief Ends a context popup opened by @ref BeginContextMenu. */
    void EndContextMenu();

    /** @brief Opens a styled popup menu. Uses ImGui::BeginPopup internally. */
    [[nodiscard]] bool BeginMenuPopup(const char *id);

    /** @brief Ends a popup menu opened by @ref BeginMenuPopup. */
    void EndMenuPopup();

    /**
     * @brief Opens a menu-bar dropdown using the shared workspace popup surface.
     * @param label Localized menu-bar label and stable ImGui identity.
     * @param fonts Editor typography handles.
     * @return True while the dropdown is open; pair with @ref EndMenuDropdown.
     */
    [[nodiscard]] bool BeginMenuDropdown(const char *label, const Theme::Fonts &fonts);

    /** @brief Ends a menu-bar dropdown opened by @ref BeginMenuDropdown. */
    void EndMenuDropdown();

    /**
     * @brief Draws one shared context-menu action row.
     * @param label Localized action label.
     * @param shortcut Optional platform shortcut label.
     * @param fonts Editor typography handles.
     * @param tone Semantic action tone.
     * @param iconToken Optional semantic icon token.
     * @param enabled Whether the action accepts input.
     * @return True when the action was activated.
     */
    [[nodiscard]] bool ContextMenuItem(const char *label, const char *shortcut, const Theme::Fonts &fonts,
                                       ContextMenuItemTone tone = ContextMenuItemTone::Normal, std::string_view iconToken = {},
                                       bool enabled = true);

    /** @brief Opens one shared nested context-menu category row. */
    [[nodiscard]] bool BeginContextSubmenu(const char *label, const Theme::Fonts &fonts, std::string_view iconToken = {});

    /** @brief Ends a nested context-menu category opened by @ref BeginContextSubmenu. */
    void EndContextSubmenu();

    /** @brief Draws a shared inset context-menu separator. */
    void ContextMenuSeparator();

    /** @brief Interaction result returned by an editable inspector property row. */
    struct PropertyEditResult {
        bool changed{false};   /**< Value changed during the current frame. */
        bool committed{false}; /**< The current edit interaction completed with a changed value. */
    };

    /** @brief Interaction result for a three-axis Inspector property row. */
    struct Float3PropertyEditResult : PropertyEditResult {
        std::array<bool, 3> changedAxes{}; /**< Axes changed during the current frame. */
    };

    /** @brief Optional interaction and presentation configuration for a floating-point property. */
    struct FloatPropertyOptions {
        float speed{0.05F};
        float minimum{0.0F};
        float maximum{0.0F};
        bool error{false};
        const char *format{"%.2f"};
    };

    /**
     * @brief Draws a shared Inspector row for selecting one localized enum entry.
     * @param label Localized property label.
     * @param id Stable UI identity scoped by the caller.
     * @param value Mutable selected entry index.
     * @param entries Localized entries in typed enum order.
     * @param fonts Editor typography handles.
     * @return True when selection changed and should be committed.
     */
    [[nodiscard]] bool DrawComboPropRow(const char *label, const char *id, int &value, std::span<const char *const> entries,
                                        const Theme::Fonts &fonts);

    /**
     * @brief Draws a shared Inspector row for editing one floating-point value.
     * @param label Localized property label.
     * @param id Stable UI identity scoped by the caller.
     * @param value Mutable floating-point draft.
     * @param fonts Editor typography handles.
     * @param options Optional drag behavior, validation state, and display format.
     * @return Per-frame change and interaction-commit state.
     */
    [[nodiscard]] PropertyEditResult DrawFloatPropRow(const char *label, const char *id, float &value, const Theme::Fonts &fonts,
                                                      FloatPropertyOptions options = {});

    /**
     * @brief Draws a shared Inspector row for editing an RGB color.
     * @param label Localized property label.
     * @param id Stable UI identity scoped by the caller.
     * @param value Mutable red, green, and blue values.
     * @param fonts Editor typography handles.
     * @param error Whether to render the control in its validation-error state.
     * @return Per-frame change and interaction-commit state.
     */
    [[nodiscard]] PropertyEditResult DrawColor3PropRow(const char *label, const char *id, std::array<float, 3> &value,
                                                       const Theme::Fonts &fonts, bool error = false);

    /**
     * @brief Draws a shared inspector row for editing three floating-point axis values.
     * @param label Localized property label.
     * @param id Stable UI identity scoped by the caller.
     * @param value Mutable X, Y, and Z values.
     * @param fonts Editor typography handles.
     * @param speed Mouse-drag increment.
     * @param mixed Axes whose selected objects currently have different values.
     * @return Per-frame change and interaction-commit state.
     */
    [[nodiscard]] Float3PropertyEditResult DrawFloat3PropRow(const char *label, const char *id, std::array<float, 3> &value,
                                                             const Theme::Fonts &fonts, float speed = 0.05F,
                                                             const std::array<bool, 3> &mixed = {});

}  // namespace Horo::Editor::Ui
