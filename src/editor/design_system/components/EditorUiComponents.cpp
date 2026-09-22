/** @copydoc EditorUiComponents.h */

#include "Horo/Editor/EditorUiComponents.h"

#include "Horo/Editor/EditorTheme.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <imgui.h>
#include <string>
#include <vector>

namespace Horo::Editor::Ui {
    namespace {
        namespace InspectorTypography {
            [[nodiscard]] float Label() {
                return Theme::TextPx::Label();
            }

            [[nodiscard]] float Field() {
                return Theme::TextPx::Body();
            }

            [[nodiscard]] float Axis() {
                return Theme::TextPx::Label();
            }

            [[nodiscard]] float ObjectTitle() {
                return Theme::TextPx::Title();
            }

            [[nodiscard]] float ObjectMeta() {
                return Theme::TextPx::Caption();
            }
        }  // namespace InspectorTypography

        namespace CardTypography {
            [[nodiscard]] float Title() {
                return Theme::TextPx::CardTitle();
            }
        }  // namespace CardTypography

        struct ResolvedPrimitiveStyle {
            DesignSystem::ComponentSizeMetrics metrics;
            ImVec2 framePadding;
            bool fillAvailableWidth{false};
        };

        [[nodiscard]] ImVec2 DefaultFramePadding(const DesignSystem::ComponentSizeMetrics &metrics,
                                                 const float renderedTextHeight = 0.0F) noexcept {
            const float textHeight = renderedTextHeight > 0.0F ? renderedTextHeight : metrics.fontSize;
            const float minimumPaddingY = std::max(0.0F, (metrics.minimumHeight - textHeight) * 0.5F);
            return {metrics.paddingX, std::max(metrics.paddingY, minimumPaddingY)};
        }

        [[nodiscard]] ResolvedPrimitiveStyle ResolvePrimitiveStyle(const ComponentSize size, const StyleProperties &style) noexcept {
            const auto &tokens = Theme::GetActiveTokens();
            const DesignSystem::ComponentSizeMetrics metrics = DesignSystem::MetricsFor(tokens, size);
            const ImVec2 defaultPadding = DefaultFramePadding(metrics);
            return ResolvedPrimitiveStyle{
                .metrics = metrics,
                .framePadding =
                    {
                        style.paddingX.has_value() ? DesignSystem::SpacingFor(tokens, *style.paddingX) : defaultPadding.x,
                        style.paddingY.has_value() ? DesignSystem::SpacingFor(tokens, *style.paddingY) : defaultPadding.y,
                    },
                .fillAvailableWidth = style.width == StyleWidth::FillAvailable,
            };
        }

        /** @brief Pushes the shared frame-state colors used by editor controls. */
        void PushControlColors(const ImVec4 background, const ImVec4 border, const ImVec4 text) {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, background);
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Hover());
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Hover());
            ImGui::PushStyleColor(ImGuiCol_Border, border);
            ImGui::PushStyleColor(ImGuiCol_Text, text);
        }

        /** @brief Pushes the shared validation-error colors for editable controls. */
        void PushErrorControlColors() {
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::ErrSoft());
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::ErrSoft());
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::ErrSoft());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Err());
        }

        void PushControlStyle() {
            const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), ComponentSize::Small);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{metrics.paddingX, metrics.paddingY});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
            PushControlColors(Theme::Bg3(), Theme::Border(), Theme::Text());
        }

        void PushInspectorControlStyle() {
            const float scale = Theme::GetActiveTokens().sizes.uiScale;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{8.0F * scale, 7.0F * scale});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
            PushControlColors(Theme::InspectorFieldSurface(), Theme::InspectorBorder(), Theme::Text());
        }

        void PopControlStyle() {
            ImGui::PopStyleColor(5);
            ImGui::PopStyleVar(3);
        }

        /** @brief Geometry shared by the editable Inspector title input and its leading icon. */
        struct EditableTitleLayout {
            ImVec2 position;
            float width{0.0F};
            float height{0.0F};
            float iconSize{0.0F};
            float controlX{0.0F};
            float controlWidth{0.0F};
            float controlOffsetY{0.0F};
            ImVec2 controlPadding;
        };

        /** @brief Resolves editable-title geometry from the current content region and theme scale. */
        [[nodiscard]] EditableTitleLayout ResolveEditableTitleLayout(const EditableTitleProps &props, const Theme::Fonts &fonts) {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const float uiScale = Theme::GetActiveTokens().sizes.uiScale;
            const float height = 38.0F * uiScale;
            const float iconSize = 22.0F * uiScale;
            const float controlX = position.x + iconSize + 7.0F * uiScale;
            const float controlWidth = std::max(1.0F, position.x + width - props.trailingWidth - 6.0F * uiScale - controlX);
            const float titleFontSize = InspectorTypography::ObjectTitle();
            const float effectiveFontHeight = fonts.sansEmphasis != nullptr
                                                  ? fonts.sansEmphasis->FontSize * Theme::Scale(titleFontSize, Theme::FontPx::SansEmphasis)
                                                  : titleFontSize;
            const float controlHeight = 30.0F * uiScale;
            return {
                .position = position,
                .width = width,
                .height = height,
                .iconSize = iconSize,
                .controlX = controlX,
                .controlWidth = controlWidth,
                .controlOffsetY = std::max(0.0F, (height - controlHeight) * 0.5F),
                .controlPadding = {8.0F * uiScale, std::max(0.0F, (controlHeight - effectiveFontHeight) * 0.5F)},
            };
        }

        /** @brief Draws the styled editable-title input and captures its interaction result. */
        [[nodiscard]] TextEditResult DrawEditableTitleInput(const char *id, std::string &value, const Theme::Fonts &fonts,
                                                            const EditableTitleProps &props, const EditableTitleLayout &layout) {
            ImGui::SetCursorScreenPos({layout.controlX, layout.position.y + layout.controlOffsetY});
            ImGui::PushID(id);
            ImGui::PushItemWidth(layout.controlWidth);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, layout.controlPadding);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
            PushControlColors(Theme::InspectorTitleFieldSurface(), Theme::InspectorBorder(), Theme::Text());
            if (props.error)
                PushErrorControlColors();
            const float titleFontSize = InspectorTypography::ObjectTitle();
            bool submitted = false;
            {
                Theme::ScopedTextStyle textStyle(fonts.sansEmphasis, titleFontSize, Theme::FontPx::SansEmphasis);
                submitted = ImGui::InputText("##value", value.data(), value.size() + 1,
                                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
            }
            const bool active = ImGui::IsItemActive();
            const bool deactivated = ImGui::IsItemDeactivated();
            const bool cancelled = (active || deactivated) && ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            const TextEditResult result{.changed = ImGui::IsItemEdited(),
                                        .committed = !cancelled && (submitted || deactivated),
                                        .cancelled = cancelled,
                                        .active = active};
            if (props.error)
                ImGui::PopStyleColor(4);
            ImGui::PopStyleColor(5);
            ImGui::PopStyleVar(3);
            ImGui::PopItemWidth();
            ImGui::PopID();
            return result;
        }

        struct PropertyRowLayout {
            ImVec2 position;
            float width{0.0F};
            float height{0.0F};
            float controlX{0.0F};
            float controlY{0.0F};
            float controlWidth{0.0F};
        };

        [[nodiscard]] PropertyRowLayout BeginPropertyRow(const char *label, const Theme::Fonts &fonts) {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const auto &tokens = Theme::GetActiveTokens();
            const float controlHeight = ScaledLayoutValue(28.0F);
            const float height = controlHeight + tokens.spacing.propertyRowGap;
            const float horizontalPadding = ScaledLayoutValue(8.0F);
            const float minimumControlWidth = ScaledLayoutValue(90.0F);
            const float labelFontSize = InspectorTypography::Label();
            const float labelWidth = std::min(ScaledLayoutValue(68.0F),
                                              std::max(ScaledLayoutValue(56.0F), width - horizontalPadding * 2.0F - minimumControlWidth));

            ImDrawList *drawList = ImGui::GetWindowDrawList();
            drawList->PushClipRect({position.x + horizontalPadding, position.y},
                                   {position.x + horizontalPadding + labelWidth - 4.0F, position.y + height}, true);
            const float labelY = position.y + (height - labelFontSize) * 0.5F;
            drawList->AddText(fonts.sans, labelFontSize, {position.x + horizontalPadding, labelY}, Theme::U32(Theme::Muted()), label);
            drawList->PopClipRect();

            const float controlX = position.x + horizontalPadding + labelWidth;
            return {
                .position = position,
                .width = width,
                .height = height,
                .controlX = controlX,
                .controlY = position.y + tokens.spacing.propertyRowGap * 0.5F,
                .controlWidth = std::max(1.0F, width - horizontalPadding * 2.0F - labelWidth),
            };
        }

        void EndPropertyRow(const PropertyRowLayout &layout) {
            ImGui::SetCursorScreenPos({layout.position.x, layout.position.y + layout.height});
        }

        /** @brief Pushes popup styling shared by context and menu-bar surfaces. */
        void PushPopupWindowStyle(const bool suppressItemSpacing) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {ScaledLayoutValue(4.0F), ScaledLayoutValue(4.0F)});
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Theme::GetActiveTokens().radii.control);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
            if (suppressItemSpacing)
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, 0.0F});
            ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::MenuSurface());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::MenuBorder());
        }

        void PushContextPopupWindowStyle() {
            PushPopupWindowStyle(true);
        }

        void PopContextPopupWindowStyle() {
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(4);
        }

        void PushMenuDropdownWindowStyle() {
            PushPopupWindowStyle(false);
        }

        void PopMenuDropdownWindowStyle() {
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar(3);
        }

        /** @brief Applies the shared compact width range to the next popup menu. */
        void SetNextContextMenuSizeConstraints() {
            constexpr float minimumWidth = 176.0F;
            constexpr float maximumWidth = 340.0F;
            ImGui::SetNextWindowSizeConstraints({ScaledLayoutValue(minimumWidth), 0.0F}, {ScaledLayoutValue(maximumWidth), FLT_MAX});
        }

        struct ContextMenuRow {
            ImVec2 minimum;
            ImVec2 maximum;
            bool activated{false};
            bool hovered{false};
        };

        /** @brief Visual content rendered over one custom context-menu row. */
        struct ContextMenuRowPresentation {
            const char *label;
            const char *shortcut;
            const Theme::Fonts &fonts;
            ImVec4 textColor;
            bool highlighted;
            bool submenu;
            std::string_view iconToken;
        };

        [[nodiscard]] ContextMenuRow DrawContextMenuRow(const char *id, const bool selected, const bool keepPopupOpen = false,
                                                        const bool enabled = true,
                                                        const ImGuiHoveredFlags hoverFlags = ImGuiHoveredFlags_None) {
            constexpr float rowHeight = 30.0F;
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{});
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4{});
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4{});
            ImGuiSelectableFlags flags = ImGuiSelectableFlags_None;
            if (keepPopupOpen)
                flags |= ImGuiSelectableFlags_NoAutoClosePopups;
            if (!enabled)
                flags |= ImGuiSelectableFlags_Disabled;
            const bool activated = ImGui::Selectable(id, selected, flags, {ImGui::GetContentRegionAvail().x, ScaledLayoutValue(rowHeight)});
            ImGui::PopStyleColor(3);
            return {
                .minimum = ImGui::GetItemRectMin(),
                .maximum = ImGui::GetItemRectMax(),
                .activated = activated,
                .hovered = ImGui::IsItemHovered(hoverFlags),
            };
        }

        void DrawContextMenuRowPresentation(const ContextMenuRow &row, const ContextMenuRowPresentation &presentation) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const float scale = Theme::GetActiveTokens().sizes.uiScale;
            const float fontSize = Theme::TextPx::Label();
            if (presentation.highlighted)
                drawList->AddRectFilled(row.minimum, row.maximum, Theme::U32(Theme::Hover()), ScaledLayoutValue(3.0F));

            const char *labelEnd = std::strstr(presentation.label, "###");
            const float textY = row.minimum.y + (row.maximum.y - row.minimum.y - fontSize) * 0.5F;
            float labelX = row.minimum.x + ScaledLayoutValue(10.0F);
            if (const std::optional<UiIcon> icon = UiIconRegistry::Resolve(presentation.iconToken); icon.has_value()) {
                constexpr float iconSize = 16.0F;
                DrawEditorIcon(drawList, *icon,
                               {labelX, row.minimum.y + (row.maximum.y - row.minimum.y - ScaledLayoutValue(iconSize)) * 0.5F},
                               {ScaledLayoutValue(iconSize), ScaledLayoutValue(iconSize)}, Theme::U32(presentation.textColor),
                               presentation.fonts.icon);
                labelX += ScaledLayoutValue(23.0F);
            }
            drawList->AddText(presentation.fonts.sans, fontSize, {labelX, textY}, Theme::U32(presentation.textColor), presentation.label,
                              labelEnd);
            if (presentation.shortcut != nullptr && presentation.shortcut[0] != '\0') {
                const ImVec2 shortcutSize = presentation.fonts.sans->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, presentation.shortcut);
                drawList->AddText(presentation.fonts.sans, fontSize, {row.maximum.x - ScaledLayoutValue(10.0F) - shortcutSize.x, textY},
                                  Theme::U32(Theme::Dim()), presentation.shortcut);
            }
            if (presentation.submenu) {
                const float arrowX = row.maximum.x - ScaledLayoutValue(10.0F);
                const float arrowY = (row.minimum.y + row.maximum.y) * 0.5F;
                drawList->AddTriangleFilled({arrowX - 3.0F * scale, arrowY - 4.0F * scale}, {arrowX - 3.0F * scale, arrowY + 4.0F * scale},
                                            {arrowX + 2.0F * scale, arrowY}, Theme::U32(Theme::Muted()));
            }
        }

        void DrawTableCell(const TableProps &props, const TableRow &row, const std::size_t rowIndex, const std::size_t columnIndex,
                           TableInteraction &interaction) {
            const TableCell *cell = columnIndex < row.cells.size() ? &row.cells[columnIndex] : nullptr;
            const std::string_view text = cell != nullptr ? std::string_view{cell->text} : std::string_view{};
            const ImVec4 color = cell != nullptr ? cell->color : Theme::Dim();
            if (!props.selectableCells) {
                ImGui::TextColored(color, "%.*s", static_cast<int>(text.size()), text.data());
                return;
            }

            ImGui::PushID(static_cast<int>(rowIndex * 1000U + columnIndex));
            ImGui::PushStyleColor(ImGuiCol_Text, color);
            if (ImGui::Selectable(text.data(), false, ImGuiSelectableFlags_AllowOverlap)) {
                interaction.activatedRow = rowIndex;
                interaction.activatedColumn = columnIndex;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }

        void DrawTableRows(const TableProps &props, const std::span<const TableColumn> columns, const std::span<const TableRow> rows,
                           const float rowHeight, TableInteraction &interaction) {
            for (std::size_t rowIndex = 0; rowIndex < rows.size(); ++rowIndex) {
                const TableRow &row = rows[rowIndex];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, rowHeight);
                std::size_t visibleCellIndex = 0;
                for (std::size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex) {
                    if (const TableColumn &column = columns[columnIndex]; column.visible) {
                        ImGui::TableSetColumnIndex(static_cast<int>(visibleCellIndex++));
                        DrawTableCell(props, row, rowIndex, columnIndex, interaction);
                    }
                }
            }
        }

        void SetupTableColumn(const TableColumn &column) {
            const ImGuiTableColumnFlags flags = column.width > 0.0F ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch;
            ImGui::TableSetupColumn(column.label.c_str(), flags, column.width);
        }
    }  // namespace

    float ScaledLayoutValue(const float value) noexcept {
        return value * Theme::GetActiveTokens().sizes.uiScale;
    }

    /** @copydoc ScopedTooltip::ScopedTooltip */
    ScopedTooltip::ScopedTooltip(const Theme::Fonts *fonts) {
        const auto &tokens = Theme::GetActiveTokens();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            {DesignSystem::SpacingFor(tokens, SpacingSize::Medium), DesignSystem::SpacingFor(tokens, SpacingSize::Small)});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, tokens.radii.card);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, std::max(1.0F, tokens.sizes.uiScale));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::TooltipSurface());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::TooltipBorder());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
        ImGui::BeginTooltip();

        if (fonts != nullptr) {
            if (fonts->sansCompact != nullptr) {
                font_ = fonts->sansCompact;
                fontScale_ = Theme::Scale(Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            } else if (fonts->sans != nullptr) {
                font_ = fonts->sans;
                fontScale_ = Theme::Scale(Theme::TextPx::Label(), Theme::FontPx::Sans);
            }
        }
        Theme::PushFont(font_);
        ImGui::SetWindowFontScale(fontScale_);
    }

    /** @copydoc ScopedTooltip::~ScopedTooltip */
    ScopedTooltip::~ScopedTooltip() {
        ImGui::SetWindowFontScale(1.0F);
        Theme::PopFont(font_);
        ImGui::EndTooltip();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(3);
    }

    /** @copydoc ShowTooltip */
    void ShowTooltip(const char *text, const Theme::Fonts *fonts) {
        if (text == nullptr || text[0] == '\0')
            return;
        ScopedTooltip tooltip(fonts);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ScaledLayoutValue(320.0F));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
    }

    // ── Button ───────────────────────────────────────────────────────────

    [[nodiscard]] bool Button(const ButtonProps &props) {
        using namespace Theme;
        const ResolvedPrimitiveStyle resolvedStyle = ResolvePrimitiveStyle(props.componentSize, props.style);

        if (!props.enabled)
            ImGui::BeginDisabled();

        if (props.variant == ButtonVariant::Primary) {
            ImGui::PushStyleColor(ImGuiCol_Button, Accent());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentHover());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, AccentActive());
            ImGui::PushStyleColor(ImGuiCol_Text, DarkText());
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, Bg3());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, AccentSoft());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4{Accent().x, Accent().y, Accent().z, 0.24F});
            ImGui::PushStyleColor(ImGuiCol_Text, Text());
        }

        ImVec2 actualSize{
            props.size.x > 0.0F ? ScaledLayoutValue(props.size.x) : props.size.x,
            props.size.y > 0.0F ? ScaledLayoutValue(props.size.y) : props.size.y,
        };
        if (actualSize.y == 0.0F)
            actualSize.y = resolvedStyle.metrics.minimumHeight;
        if (resolvedStyle.fillAvailableWidth)
            actualSize.x = ImGui::GetContentRegionAvail().x;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, resolvedStyle.framePadding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, GetActiveTokens().radii.control);

        bool clicked = false;
        {
            ScopedTextStyle textStyle(props.font, resolvedStyle.metrics.fontSize, props.baseFontSize);
            clicked = ImGui::Button(props.label, actualSize);
        }

        ImGui::PopStyleColor(4);
        if (!props.enabled)
            ImGui::EndDisabled();
        ImGui::PopStyleVar(2);
        return props.enabled && clicked;
    }

    TableInteraction DrawTable(const TableProps &props, const std::span<const TableColumn> columns, const std::span<const TableRow> rows,
                               const Theme::Fonts &fonts) {
        TableInteraction interaction;
        std::size_t visibleColumnCount = 0;
        for (const TableColumn &column : columns)
            visibleColumnCount += column.visible ? 1U : 0U;
        if (visibleColumnCount == 0U)
            return interaction;

        const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), props.componentSize);
        const float rowHeight = metrics.minimumHeight;
        const float cellPadX = metrics.paddingX;
        const float cellPadY = metrics.paddingY;
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {cellPadX, cellPadY});
        ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::U32(Theme::Bg2()));
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::U32(Theme::Dim()));
        ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, Theme::U32(Theme::Border()));
        ImGui::PushStyleColor(ImGuiCol_TableBorderLight, Theme::U32(Theme::Border()));
        ImGui::PushStyleColor(ImGuiCol_Header, Theme::U32(Theme::Hover()));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::U32(Theme::Hover()));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::U32(Theme::AccentSoft()));

        if (const ImGuiTableFlags flags =
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX | ImGuiTableFlags_ScrollY;
            ImGui::BeginTable(props.id, static_cast<int>(visibleColumnCount), flags)) {
            for (const TableColumn &column : columns) {
                if (!column.visible)
                    continue;
                SetupTableColumn(column);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {cellPadX, cellPadY});
            ImGui::TableHeadersRow();
            ImGui::PopStyleVar();

            DrawTableRows(props, columns, rows, rowHeight, interaction);
            ImGui::EndTable();
        }
        ImGui::PopStyleColor(7);
        ImGui::PopStyleVar();
        static_cast<void>(fonts);
        return interaction;
    }

    // ── ScopedCard ───────────────────────────────────────────────────────

    ScopedCard::ScopedCard(const char *id, const ImVec2 size, const float padX, const float padY, const ImVec4 bg, const bool autoResizeY) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{padX, padY});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGuiChildFlags childFlags = ImGuiChildFlags_Borders;
        if (autoResizeY) {
            childFlags |= (ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize);
        }
        ImGui::BeginChild(id, size, childFlags, ImGuiWindowFlags_NoScrollbar);
    }

    ScopedCard::~ScopedCard() {
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ── IconCloseButton ──────────────────────────────────────────────────

    [[nodiscard]] bool IconCloseButton(const char *id, const ImVec2 size) {
        using namespace Theme;

        ImGui::PushID(id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool clicked = ImGui::InvisibleButton("##close", size);
        const bool hovered = ImGui::IsItemHovered();

        auto *dl = ImGui::GetWindowDrawList();
        constexpr float pad = 4.0F;
        const ImVec2 a{pos.x + pad, pos.y + pad};
        const ImVec2 b{pos.x + size.x - pad, pos.y + size.y - pad};
        const ImU32 col = U32(hovered ? Text() : Dim());
        dl->AddLine(a, b, col, 1.5F);
        dl->AddLine({b.x, a.y}, {a.x, b.y}, col, 1.5F);

        ImGui::PopID();
        return clicked;
    }

    /** @copydoc NavigationIconButton */
    [[nodiscard]] bool NavigationIconButton(const char *id, const NavigationIcon icon, const ImVec2 size, const bool enabled) {
        using namespace Theme;

        ImGui::PushID(id);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton("##navigation", size);
        const bool hovered = enabled && ImGui::IsItemHovered();
        const bool active = enabled && ImGui::IsItemActive();
        ImGui::EndDisabled();

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (hovered || active) {
            ImVec4 surface = active ? Accent() : AccentSoft();
            surface.w = active ? 0.22F : 0.16F;
            drawList->AddRectFilled(position, {position.x + size.x, position.y + size.y}, U32(surface), 4.0F);
        }

        ImVec4 iconColor = Theme::Dim();
        if (enabled) {
            iconColor = hovered ? Theme::Text() : Theme::Muted();
        } else {
            iconColor.w *= 0.48F;
        }
        const ImU32 color = U32(iconColor);
        const ImVec2 center{
            position.x + size.x * 0.5F,
            position.y + size.y * 0.5F,
        };
        constexpr float halfWidth = 3.0F;
        constexpr float halfHeight = 5.0F;
        constexpr float thickness = 1.5F;
        if (icon == NavigationIcon::Back) {
            drawList->AddLine({center.x + halfWidth, center.y - halfHeight}, {center.x - halfWidth, center.y}, color, thickness);
            drawList->AddLine({center.x - halfWidth, center.y}, {center.x + halfWidth, center.y + halfHeight}, color, thickness);
        } else if (icon == NavigationIcon::Forward) {
            drawList->AddLine({center.x - halfWidth, center.y - halfHeight}, {center.x + halfWidth, center.y}, color, thickness);
            drawList->AddLine({center.x + halfWidth, center.y}, {center.x - halfWidth, center.y + halfHeight}, color, thickness);
        } else {
            drawList->AddLine({center.x - halfHeight, center.y + halfWidth}, {center.x, center.y - halfWidth}, color, thickness);
            drawList->AddLine({center.x, center.y - halfWidth}, {center.x + halfHeight, center.y + halfWidth}, color, thickness);
        }

        ImGui::PopID();
        return enabled && clicked;
    }

    /** @copydoc IconButton */
    bool IconButton(const IconButtonProps &props) {
        const auto &tokens = Theme::GetActiveTokens();
        const auto &metrics = DesignSystem::MetricsFor(tokens, props.componentSize);
        const float widthVal = props.size.x > 0.0F ? ScaledLayoutValue(props.size.x) : metrics.minimumHeight;
        const float heightVal = props.size.y > 0.0F ? ScaledLayoutValue(props.size.y) : metrics.minimumHeight;
        const ImVec2 size{widthVal, heightVal};
        ImGui::PushID(props.id);
        const ImVec2 position = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(!props.enabled);
        const bool clicked = ImGui::InvisibleButton("##icon-button", size);
        const bool hovered = props.enabled && ImGui::IsItemHovered();
        const bool active = props.enabled && ImGui::IsItemActive();
        const bool focused = props.enabled && ImGui::IsItemFocused();
        ImGui::EndDisabled();

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        ImVec4 surfaceColor = Theme::Bg3();
        if (active)
            surfaceColor = Theme::AccentSoft();
        else if (hovered || focused)
            surfaceColor = Theme::Hover();
        drawList->AddRectFilled(position, {position.x + size.x, position.y + size.y}, Theme::U32(surfaceColor),
                                Theme::GetActiveTokens().radii.control);
        drawList->AddRect(position, {position.x + size.x, position.y + size.y},
                          Theme::U32(hovered || active || focused ? Theme::Accent() : Theme::Border()),
                          Theme::GetActiveTokens().radii.control, 0, hovered || active || focused ? 1.5F : 1.0F);

        ImVec4 iconTone = hovered || active || focused ? Theme::Accent() : Theme::Text();
        if (!props.enabled)
            iconTone.w *= 0.45F;
        const ImU32 iconColor = Theme::U32(iconTone);
        const ImVec2 center{
            position.x + size.x * 0.5F,
            position.y + size.y * 0.5F,
        };
        const float iconHalfExtent = metrics.iconSize * 0.31F;
        const float thickness = std::max(1.0F, 1.5F * tokens.sizes.uiScale);
        if (props.glyph == IconButtonGlyph::Plus) {
            drawList->AddLine({center.x - iconHalfExtent, center.y}, {center.x + iconHalfExtent, center.y}, iconColor, thickness);
            drawList->AddLine({center.x, center.y - iconHalfExtent}, {center.x, center.y + iconHalfExtent}, iconColor, thickness);
        } else {
            const float iconWidth = metrics.iconSize;
            const float iconHeight = metrics.iconSize * 0.67F;
            const ImVec2 origin{center.x - iconWidth * 0.5F, center.y - iconHeight * 0.5F};
            drawList->AddLine(origin, {origin.x + iconWidth / 3.0F, origin.y}, iconColor, thickness);
            drawList->AddLine({origin.x + iconWidth / 3.0F, origin.y}, {origin.x + iconWidth * 0.44F, origin.y + iconHeight / 6.0F},
                              iconColor, thickness);
            drawList->AddRect({origin.x, origin.y + iconHeight / 6.0F}, {origin.x + iconWidth, origin.y + iconHeight}, iconColor,
                              metrics.iconSize * 0.11F, 0, thickness);
            drawList->AddLine({origin.x, origin.y + iconHeight * 0.38F}, {origin.x + iconWidth, origin.y + iconHeight * 0.38F}, iconColor,
                              thickness);
        }

        if (hovered && props.tooltip != nullptr && props.tooltip[0] != '\0')
            ShowTooltip(props.tooltip);
        ImGui::PopID();
        return props.enabled && clicked;
    }

    // ── SectionTitle ─────────────────────────────────────────────────────

    void SectionTitle(const char *upperCaseLabel, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Title(), Theme::FontPx::SansCompact);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
        ImGui::TextUnformatted(upperCaseLabel);
        ImGui::PopStyleColor();
    }

    // ── FieldLabel ───────────────────────────────────────────────────────

    void FieldLabel(const char *upperCaseLabel, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
        ImGui::TextUnformatted(upperCaseLabel);
        ImGui::PopStyleColor();
    }

    // ── Hint ─────────────────────────────────────────────────────────────

    void Hint(const char *text, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    // ── ErrorText ────────────────────────────────────────────────────────

    void ErrorText(const char *text, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Err());
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
        ImGui::TextWrapped("%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }

    /** @copydoc TextLink */
    bool TextLink(const char *id, const char *label, ImFont *font, const float fontSize, const bool current) {
        ImFont *resolvedFont = font != nullptr ? font : ImGui::GetFont();
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const ImVec2 textSize = resolvedFont->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, label);
        const ImVec2 hitSize{textSize.x, std::max(textSize.y, 20.0F)};
        const bool activated = ImGui::InvisibleButton(id, hitSize);
        const bool hovered = ImGui::IsItemHovered();
        const bool focused = ImGui::IsItemFocused();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        ImVec4 textColor = current ? Theme::Text() : Theme::Muted();
        if (hovered || focused)
            textColor = Theme::Accent();
        const ImU32 color = Theme::U32(textColor);
        const ImVec2 textPosition{position.x, position.y + (hitSize.y - textSize.y) * 0.5F};
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddText(resolvedFont, fontSize, textPosition, color, label);
        if (hovered || focused) {
            drawList->AddLine({textPosition.x, textPosition.y + textSize.y + 1.0F},
                              {textPosition.x + textSize.x, textPosition.y + textSize.y + 1.0F}, color, 1.0F);
        }
        return activated;
    }

    // ── Badge / tag ──────────────────────────────────────────────────────

    /** @copydoc BadgeToneColor */
    ImVec4 BadgeToneColor(const BadgeTone tone) {
        switch (tone) {
            case BadgeTone::Accent:
                return Theme::Accent();
            case BadgeTone::Success:
                return Theme::Ok();
            case BadgeTone::Warning:
                return Theme::Warn();
            case BadgeTone::Error:
                return Theme::Err();
            case BadgeTone::Neutral:
            default:
                return Theme::Muted();
        }
    }

    namespace {
        [[nodiscard]] ImVec2 MeasureBadge(const BadgeProps &props, const Theme::Fonts &fonts) {
            using enum BadgeSize;
            const float horizontalPadding = props.size == Medium ? 12.0F : 9.0F;
            const float verticalPadding = props.size == Medium ? 9.0F : 4.0F;
            constexpr float indicatorWidth = 12.0F;
            Theme::ScopedTextStyle textStyle(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            const ImVec2 textSize = ImGui::CalcTextSize(props.label);
            return {
                textSize.x + horizontalPadding * 2.0F + (props.leadingIndicator ? indicatorWidth : 0.0F),
                textSize.y + verticalPadding * 2.0F,
            };
        }
    }  // namespace

    /** @copydoc BadgeWidth */
    float BadgeWidth(const BadgeProps &props, const Theme::Fonts &fonts) {
        return MeasureBadge(props, fonts).x;
    }

    /** @copydoc Badge */
    void Badge(const BadgeProps &props, const Theme::Fonts &fonts) {
        const float horizontalPadding = props.size == BadgeSize::Medium ? 12.0F : 9.0F;
        const float verticalPadding = props.size == BadgeSize::Medium ? 9.0F : 4.0F;
        constexpr float radius = 4.0F;
        constexpr float inlineGap = 6.0F;
        const ImVec4 color = BadgeToneColor(props.tone);
        Theme::ScopedTextStyle textStyle(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
        const ImVec2 textSize = ImGui::CalcTextSize(props.label);
        const ImVec2 badgeMin = ImGui::GetCursorScreenPos();
        const ImVec2 badgeSize{
            textSize.x + horizontalPadding * 2.0F + (props.leadingIndicator ? 12.0F : 0.0F),
            textSize.y + verticalPadding * 2.0F,
        };
        const ImVec2 badgeMax{badgeMin.x + badgeSize.x, badgeMin.y + badgeSize.y};
        float textX = badgeMin.x + horizontalPadding;
        ImDrawList *drawList = ImGui::GetWindowDrawList();

        ImVec4 surface = color;
        surface.w = 0.10F;
        ImVec4 border = color;
        border.w = 0.28F;
        drawList->AddRectFilled(badgeMin, badgeMax, Theme::U32(surface), radius);
        drawList->AddRect(badgeMin, badgeMax, Theme::U32(border), radius);
        if (props.leadingIndicator) {
            constexpr float indicatorRadius = 3.0F;
            const ImVec2 center{
                badgeMin.x + horizontalPadding + indicatorRadius,
                badgeMin.y + badgeSize.y * 0.5F,
            };
            drawList->AddCircleFilled(center, indicatorRadius, Theme::U32(color));
            textX += 12.0F;
        }
        const ImVec2 textPosition{
            textX,
            badgeMin.y + (badgeSize.y - textSize.y) * 0.5F,
        };
        drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize(), textPosition, Theme::U32(color), props.label);
        ImGui::Dummy(badgeSize);
        ImGui::SetCursorScreenPos({badgeMax.x + inlineGap, badgeMin.y});
    }

    // ── DashedSeparator ──────────────────────────────────────────────────

    void DashedSeparator(const float dash, const float gap) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        auto *dl = ImGui::GetWindowDrawList();
        float x = p.x;
        while (x < p.x + w) {
            const float end = std::min(x + dash, p.x + w);
            dl->AddLine({x, p.y}, {end, p.y}, Theme::U32(Theme::Border()), 1.0F);
            x = end + gap;
        }
        ImGui::Dummy({0.0F, 4.0F});
    }

    /** @copydoc LabeledSeparator */
    void LabeledSeparator(const char *label, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
        const ImVec2 textSize = ImGui::CalcTextSize(label);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        const float lineStart = maximum.x + 12.0F;
        const float lineEnd = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        if (lineEnd > lineStart) {
            ImGui::GetWindowDrawList()->AddLine({lineStart, minimum.y + textSize.y * 0.5F}, {lineEnd, minimum.y + textSize.y * 0.5F},
                                                Theme::U32(Theme::Border()), 1.0F);
        }
    }

    // ── SettingGroup ─────────────────────────────────────────────────────

    void SettingGroup(const char *label, const Theme::Fonts &fonts, const bool first) {
        if (!first) {
            ImGui::Dummy({0.0F, ScaledLayoutValue(18.0F)});
        }

        Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(p, {p.x + w, p.y}, Theme::U32(Theme::Border()), 1.0F);
        ImGui::Dummy({0.0F, ScaledLayoutValue(8.0F)});
    }

    // ── ComboControl ─────────────────────────────────────────────────────

    bool DrawComboRow(const int index, int *value, const ComboItemSource &source, const Theme::Fonts &fonts) {
        ImGui::PushID(index);
        const bool isSelected = (*value == index);
        const bool isEnabled = !source.enabled || source.enabled(index);
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        const float rowH = ScaledLayoutValue(28.0F);
        const float rowW = ImGui::GetContentRegionAvail().x;
        const std::string rowId = std::format("{}###combo_option_{}", source.label(index), index);
        ImGui::InvisibleButton(rowId.c_str(), {rowW, rowH});
        const bool rowHovered = ImGui::IsItemHovered();
        const bool clicked = isEnabled && ImGui::IsItemClicked();
        if (clicked) {
            *value = index;
            ImGui::CloseCurrentPopup();
        }
        auto *drawList = ImGui::GetWindowDrawList();
        if (rowHovered || isSelected)
            drawList->AddRectFilled(rowMin, {rowMin.x + rowW, rowMin.y + rowH}, Theme::U32(Theme::Hover()));
        const float rowFontSize = Theme::TextPx::Label();
        ImVec4 rowTextColor = Theme::Dim();
        if (isEnabled)
            rowTextColor = isSelected ? Theme::Text() : Theme::Muted();
        drawList->AddText(fonts.sansCompact ? fonts.sansCompact : ImGui::GetFont(), rowFontSize,
                          {rowMin.x + ScaledLayoutValue(14.0F), rowMin.y + (rowH - rowFontSize) * 0.5F}, Theme::U32(rowTextColor),
                          source.label(index));
        if (!isEnabled && rowHovered && source.disabledTooltip) {
            const char *const tooltip = source.disabledTooltip(index);
            if (tooltip != nullptr && tooltip[0] != '\0') {
                ShowTooltip(tooltip, &fonts);
            }
        }
        ImGui::PopID();
        return clicked;
    }

    bool ComboControl(const char *id, int *value, const char *const *items, const int itemCount, const Theme::Fonts &fonts,
                      const ComboControlOptions options) {
        const ComboItemSource source{.label = [items](const int index) {
            return items[index];
        }};
        return ComboControl(id, value, itemCount, source, fonts, options);
    }

    bool ComboControl(const char *id, int *value, const int itemCount, const ComboItemSource &source, const Theme::Fonts &fonts,
                      const ComboControlOptions options) {
        IM_ASSERT(source.label);
        bool changed = false;
        ImGui::PushID(id);

        const auto &tokens = Theme::GetActiveTokens();
        const auto &sizeTokens = tokens.sizes;
        const auto &metrics = DesignSystem::MetricsFor(tokens, options.componentSize);
        const float scale = sizeTokens.uiScale;
        const float fieldW = ImGui::CalcItemWidth();
        const float fieldH = options.height > 0.0F ? options.height * scale : metrics.minimumHeight;

        const ImVec2 fieldPos = ImGui::GetCursorScreenPos();
        const std::string fieldId = std::string("Combo###") + id;
        ImGui::InvisibleButton(fieldId.c_str(), ImVec2{fieldW, fieldH});
        const bool fieldHovered = ImGui::IsItemHovered();
        const bool fieldClicked = ImGui::IsItemClicked();

        const std::string popupId = std::string("##popup_") + id;
        const bool popupOpen = ImGui::IsPopupOpen(popupId.c_str());

        auto *dl = ImGui::GetWindowDrawList();
        const ImVec4 controlSurface =
            options.surface == ComboControlSurface::BottomDockToolbar ? Theme::BottomDockControlSurface() : Theme::Bg3();
        dl->AddRectFilled(fieldPos, {fieldPos.x + fieldW, fieldPos.y + fieldH}, Theme::U32(fieldHovered ? Theme::Hover() : controlSurface),
                          Theme::GetActiveTokens().radii.control);
        const ImVec4 borderColor = options.error ? Theme::Err() : Theme::Border();
        const ImVec4 popupBorderColor = popupOpen ? Theme::Accent() : borderColor;
        dl->AddRect(fieldPos, {fieldPos.x + fieldW, fieldPos.y + fieldH}, Theme::U32(popupBorderColor),
                    Theme::GetActiveTokens().radii.control, 0, popupOpen ? 1.5F : 1.0F);

        // Selected value label
        {
            ImFont *font = fonts.sansCompact ? fonts.sansCompact : ImGui::GetFont();
            const char *label = (*value >= 0 && *value < itemCount) ? source.label(*value) : "";
            dl->AddText(font, metrics.fontSize, {fieldPos.x + metrics.paddingX, fieldPos.y + (fieldH - metrics.fontSize) * 0.5F},
                        Theme::U32(Theme::Text()), label);
        }

        // Right-side arrow
        {
            const float cx = fieldPos.x + fieldW - 18.0F;
            const float cy = fieldPos.y + fieldH * 0.5F;
            const ImU32 arrowCol = Theme::U32(fieldHovered ? Theme::Text() : Theme::Muted());
            dl->AddTriangleFilled({cx - 4.0F, cy - 2.0F}, {cx + 4.0F, cy - 2.0F}, {cx, cy + 3.0F}, arrowCol);
        }

        if (fieldClicked)
            ImGui::OpenPopup(popupId.c_str());

        ImGui::SetNextWindowPos({fieldPos.x, fieldPos.y + fieldH + 4.0F});
        ImGui::SetNextWindowSize({fieldW, 0.0F});

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 5.0F * scale});
        ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 6.0F * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::BorderStrong());

        if (ImGui::BeginPopup(popupId.c_str(), ImGuiWindowFlags_NoMove)) {
            const ImVec2 pMin = ImGui::GetWindowPos();
            const ImVec2 pMax = {pMin.x + ImGui::GetWindowWidth(), pMin.y + ImGui::GetWindowHeight()};

            auto *bgdl = ImGui::GetBackgroundDrawList();
            constexpr int shadowLayers = 12;
            for (int i = shadowLayers; i >= 1; --i) {
                const float t = static_cast<float>(i) / static_cast<float>(shadowLayers);
                const float spread = 16.0F * t;
                const float alpha = 0.45F * (1.0F - t) * 0.11F;
                bgdl->AddRectFilled({pMin.x - spread, pMin.y + 3.0F - spread * 0.25F}, {pMax.x + spread, pMax.y + 3.0F + spread},
                                    Theme::U32(ImVec4{0.0F, 0.0F, 0.0F, alpha}), 6.0F + spread);
            }

            for (int i = 0; i < itemCount; ++i)
                changed = DrawComboRow(i, value, source, fonts) || changed;
            ImGui::EndPopup();
        }

        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(4);
        ImGui::PopID();
        return changed;
    }

    // ── InputTextControl ─────────────────────────────────────────────────

    /** @copydoc InputTextControl(const char *, char *, size_t, const Theme::Fonts &, const InputTextOptions &) */
    bool InputTextControl(const char *id, char *buffer, const size_t bufferSize, const Theme::Fonts &fonts,
                          const InputTextOptions &options) {
        const auto &tokens = Theme::GetActiveTokens();
        const auto &metrics = DesignSystem::MetricsFor(tokens, options.componentSize);
        const float renderedTextHeight =
            fonts.sansCompact != nullptr ? fonts.sansCompact->FontSize * metrics.fontSize / Theme::FontPx::SansCompact : metrics.fontSize;
        const ImVec2 framePadding = DefaultFramePadding(metrics, renderedTextHeight);
        const float leftPadding = framePadding.x + options.prefixIconWidth * tokens.sizes.uiScale;
        const ImVec4 surface = options.surface == InputTextSurface::BottomDockToolbar ? Theme::BottomDockControlSurface() : Theme::Bg3();
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{leftPadding, framePadding.y});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, options.error ? Theme::ErrSoft() : surface);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, options.error ? Theme::ErrSoft() : Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, options.error ? Theme::ErrSoft() : surface);
        ImGui::PushStyleColor(ImGuiCol_Border, options.error ? Theme::Err() : Theme::Border());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());

        bool changed = false;
        ImGui::PushItemWidth(options.width > 0.0F ? ScaledLayoutValue(options.width) : options.width);
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, metrics.fontSize, Theme::FontPx::SansCompact);
            if (options.hint != nullptr)
                changed = ImGui::InputTextWithHint(id, options.hint, buffer, bufferSize);
            else
                changed = ImGui::InputText(id, buffer, bufferSize);
        }
        ImGui::PopItemWidth();

        if (ImGui::IsItemActive()) {
            const ImVec2 pMin = ImGui::GetItemRectMin();
            const ImVec2 pMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(pMin, pMax, Theme::U32(options.error ? Theme::ErrSoft() : Theme::AccentSoft()),
                                                Theme::GetActiveTokens().radii.control + 2.0F, 0, 2.0F);
        } else if (ImGui::IsItemHovered()) {
            const ImVec2 pMin = ImGui::GetItemRectMin();
            const ImVec2 pMax = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddRect(pMin, pMax, Theme::U32(Theme::BorderStrong()), Theme::GetActiveTokens().radii.control, 0,
                                                1.0F);
        }

        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        return changed;
    }

    /** @copydoc InputTextControl(const char *, std::string &, size_t, const Theme::Fonts &, const InputTextOptions &) */
    bool InputTextControl(const char *id, std::string &value, const size_t maxSize, const Theme::Fonts &fonts,
                          const InputTextOptions &options) {
        if (maxSize == 0)
            return false;
        value.resize(std::min(value.size(), maxSize - 1));
        value.resize(maxSize - 1, '\0');
        const bool changed = InputTextControl(id, value.data(), value.size() + 1, fonts, options);
        const auto nullPos = value.find('\0');
        value.resize(nullPos == std::string::npos ? value.size() : nullPos);
        return changed;
    }

    /** @copydoc SelectableTextBlock */
    bool SelectableTextBlock(const char *id, char *buffer, const size_t bufferSize,
                             const std::span<const SelectableTextLineLayout> lineLayouts, const float alignedColumnX, const float width) {
        if (buffer == nullptr || bufferSize == 0U)
            return false;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0.0F, 1.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0F);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, Theme::AccentSoft());

        const float lineHeight = ImGui::GetFontSize();
        const float blockHeight = std::max(lineHeight + 2.0F, lineHeight * static_cast<float>(lineLayouts.size()) + 2.0F);
        const float resolvedWidth = width < 0.0F ? ImGui::GetContentRegionAvail().x : width;
        static_cast<void>(ImGui::InputTextMultiline(id, buffer, bufferSize, {resolvedWidth, blockHeight},
                                                    ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoHorizontalScroll));
        const bool active = ImGui::IsItemActive();
        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 textOrigin{itemMin.x, itemMin.y + 1.0F};

        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(3);

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const float visibleMinY = ImGui::GetWindowPos().y;
        const float visibleMaxY = visibleMinY + ImGui::GetWindowHeight();
        drawList->PushClipRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), true);

        const auto *lineBegin = buffer;
        const auto *textEnd = std::find(buffer, buffer + bufferSize, '\0');
        for (std::size_t lineIndex = 0; lineIndex < lineLayouts.size(); ++lineIndex) {
            const auto lineEnd = std::find(lineBegin, textEnd, '\n');

            if (const float lineY = textOrigin.y + lineHeight * static_cast<float>(lineIndex);
                lineY + lineHeight >= visibleMinY && lineY <= visibleMaxY) {
                const auto &layout = lineLayouts[lineIndex];
                const auto color = ImGui::ColorConvertFloat4ToU32(layout.color);
                const auto lineByteCount = static_cast<std::size_t>(lineEnd - lineBegin);
                const std::size_t columnOffset = std::min(layout.alignedColumnByteOffset, lineByteCount);
                if (columnOffset > 0U && alignedColumnX > 0.0F) {
                    const char *columnBegin = lineBegin + columnOffset;
                    drawList->AddText(ImGui::GetFont(), lineHeight, {textOrigin.x, lineY}, color, lineBegin, columnBegin);
                    drawList->AddText(ImGui::GetFont(), lineHeight, {textOrigin.x + alignedColumnX, lineY}, color, columnBegin, lineEnd);
                } else {
                    drawList->AddText(ImGui::GetFont(), lineHeight, {textOrigin.x, lineY}, color, lineBegin, lineEnd);
                }
            }

            if (lineEnd == textEnd)
                break;
            lineBegin = lineEnd + 1;
        }
        drawList->PopClipRect();
        return active;
    }

    bool ColorHexControl(const char *id, std::string &value, const size_t maxSize, const Theme::Fonts &fonts) {
        if (maxSize == 0)
            return false;
        value.resize(std::min(value.size(), maxSize - 1));
        value.resize(maxSize - 1, '\0');
        const bool changed = ColorHexControl(id, value.data(), value.size() + 1, fonts);
        const auto nullPos = value.find('\0');
        value.resize(nullPos == std::string::npos ? value.size() : nullPos);
        return changed;
    }

    // ── ColorHexControl ───────────────────────────────────────────────────

    [[nodiscard]] int HexDigit(const char value) {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    }

    [[nodiscard]] bool ParseHexColor(const char *text, ImVec4 &out) {
        if (text == nullptr || text[0] != '#' || text[7] != '\0')
            return false;
        const std::array<int, 6> digits = {HexDigit(text[1]), HexDigit(text[2]), HexDigit(text[3]),
                                           HexDigit(text[4]), HexDigit(text[5]), HexDigit(text[6])};
        for (const int digit : digits)
            if (digit < 0)
                return false;
        out = ImVec4{static_cast<float>(digits[0] * 16 + digits[1]) / 255.0F, static_cast<float>(digits[2] * 16 + digits[3]) / 255.0F,
                     static_cast<float>(digits[4] * 16 + digits[5]) / 255.0F, 1.0F};
        return true;
    }

    void WriteCanonicalColor(char *buffer, const size_t bufferSize, const ImVec4 color) {
        if (buffer == nullptr || bufferSize == 0)
            return;
        const auto red = static_cast<int>(color.x * 255.0F + 0.5F);
        const auto green = static_cast<int>(color.y * 255.0F + 0.5F);
        const auto blue = static_cast<int>(color.z * 255.0F + 0.5F);
        const std::string value = std::format("#{:02X}{:02X}{:02X}", red, green, blue);
        const std::size_t count = std::min(bufferSize - 1, value.size());
        std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
    }

    bool ColorHexControl(const char *id, char *buffer, const size_t bufferSize, const Theme::Fonts &fonts) {
        const auto pack = [](const ImVec4 color) {
            return ImGui::ColorConvertFloat4ToU32(color);
        };
        const auto unpack = [](const ImU32 color) {
            return ImGui::ColorConvertU32ToFloat4(color);
        };

        ImGui::PushID(id);
        ImGuiStorage *const storage = ImGui::GetStateStorage();
        const ImGuiID lastValidKey = ImGui::GetID("last-valid-color");
        ImVec4 current{};
        if (ParseHexColor(buffer, current)) {
            storage->SetInt(lastValidKey, static_cast<int>(pack(current)));
        } else if (storage->GetInt(lastValidKey, 0) != 0) {
            current = unpack(static_cast<ImU32>(storage->GetInt(lastValidKey)));
        } else {
            current = Theme::Accent();
            storage->SetInt(lastValidKey, static_cast<int>(pack(current)));
        }

        const ImVec2 swatchPosition = ImGui::GetCursorScreenPos();
        constexpr ImVec2 swatchSize{34.0F, 34.0F};
        ImGui::InvisibleButton("swatch", swatchSize);
        const bool openPicker = ImGui::IsItemClicked();
        ImDrawList *const drawList = ImGui::GetWindowDrawList();
        const ImVec2 swatchEnd{swatchPosition.x + swatchSize.x, swatchPosition.y + swatchSize.y};
        drawList->AddRectFilled(swatchPosition, swatchEnd, ImGui::ColorConvertFloat4ToU32(current), Theme::GetActiveTokens().radii.control);
        drawList->AddRect(swatchPosition, swatchEnd, Theme::U32(Theme::Border()), Theme::GetActiveTokens().radii.control);
        if (openPicker)
            ImGui::OpenPopup("picker");

        ImGui::SameLine(0.0F, 8.0F);
        PushControlStyle();
        ImGui::PushItemWidth(-1.0F);
        bool validChange = false;
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            if (ImGui::InputText("hex", buffer, bufferSize) && ParseHexColor(buffer, current)) {
                storage->SetInt(lastValidKey, static_cast<int>(pack(current)));
                validChange = true;
            }
        }
        ImGui::PopItemWidth();
        PopControlStyle();

        if (ImGui::BeginPopup("picker")) {
            ImGui::TextUnformatted("Accent color");
            ImGui::Separator();
            if (ImGui::ColorPicker3("##color-picker", &current.x, ImGuiColorEditFlags_NoSidePreview)) {
                WriteCanonicalColor(buffer, bufferSize, current);
                storage->SetInt(lastValidKey, static_cast<int>(pack(current)));
                validChange = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
        return validChange;
    }

    // ── InputIntControl ──────────────────────────────────────────────────

    void InputIntControl(const char *id, int *value, const Theme::Fonts &fonts) {
        PushControlStyle();
        ImGui::PushItemWidth(-1.0F);
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Body(), Theme::FontPx::SansCompact);
            ImGui::InputInt(id, value, 1, 4);
        }
        ImGui::PopItemWidth();
        PopControlStyle();
    }

    // ── InputFloatControl ────────────────────────────────────────────────

    void InputFloatControl(const char *id, float *value, const Theme::Fonts &fonts) {
        PushControlStyle();
        ImGui::PushItemWidth(-1.0F);
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Body(), Theme::FontPx::SansCompact);
            ImGui::InputFloat(id, value, 0.1F, 1.0F, "%.1f");
        }
        ImGui::PopItemWidth();
        PopControlStyle();
    }

    // ── SliderIntControl ─────────────────────────────────────────────────

    void SliderIntControl(const char *id, int *value, const int minValue, const int maxValue, const SliderValueFormat format,
                          const Theme::Fonts &fonts, const int step) {
        ImGui::PushID(id);
        const float TrackW = ScaledLayoutValue(Theme::Layout::ControlW - 54.0F);
        const float TrackH = ScaledLayoutValue(4.0F);
        const float HitH = ScaledLayoutValue(22.0F);
        const float KnobR = ScaledLayoutValue(7.0F);

        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("slider", {TrackW, HitH});
        const bool active = ImGui::IsItemActive();
        const bool hovered = ImGui::IsItemHovered();

        if (active && maxValue > minValue) {
            const float mouseT = (ImGui::GetIO().MousePos.x - pos.x) / TrackW;
            const float clampedT = std::clamp(mouseT, 0.0F, 1.0F);
            const float rawValue = static_cast<float>(minValue) + clampedT * static_cast<float>(maxValue - minValue);
            const int snapped =
                minValue + static_cast<int>(std::round((rawValue - static_cast<float>(minValue)) / static_cast<float>(step))) * step;
            *value = std::clamp(snapped, minValue, maxValue);
        }

        float t = 0.0F;
        if (maxValue > minValue) {
            t = static_cast<float>(*value - minValue) / static_cast<float>(maxValue - minValue);
        }
        const float trackY = pos.y + (HitH - TrackH) * 0.5F;
        auto *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled({pos.x, trackY}, {pos.x + TrackW, trackY + TrackH}, Theme::U32(Theme::BorderStrong()), 2.0F);
        dl->AddRectFilled({pos.x, trackY}, {pos.x + TrackW * t, trackY + TrackH}, Theme::U32(Theme::Accent()), 2.0F);

        const ImVec2 knob{pos.x + TrackW * t, pos.y + HitH * 0.5F};
        dl->AddCircleFilled(knob, KnobR + (hovered || active ? 1.0F : 0.0F), Theme::U32(Theme::AccentHover()), 20);
        dl->AddCircleFilled(knob, KnobR, Theme::U32(Theme::Accent()), 20);
        dl->AddCircle(knob, KnobR + 1.0F, Theme::U32(Theme::Bg1()), 20, 2.0F);

        ImGui::SameLine(0.0F, 10.0F);

        std::string text;
        using enum SliderValueFormat;
        switch (format) {
            case Minutes:
                text = std::format("{} min", *value);
                break;
            case Percent:
                text = std::format("{}%", *value);
                break;
            case Milliseconds:
                text = std::format("{} ms", *value);
                break;
            case Integer:
                text = std::format("{}", *value);
                break;
        }
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Body(), Theme::FontPx::SansCompact);
            const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (HitH - textSize.y) * 0.5F);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            ImGui::TextUnformatted(text.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    // ── ToggleControl ────────────────────────────────────────────────────

    [[nodiscard]] bool ToggleControl(const char *id, bool *value, const Theme::Fonts &fonts, const bool showLabel) {
        ImGui::PushID(id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        constexpr ImVec2 size{36.0F, 20.0F};
        const ImVec2 scaledSize{size.x * scale, size.y * scale};
        ImGui::InvisibleButton("toggle", scaledSize);
        const bool clicked = ImGui::IsItemClicked();
        if (clicked) {
            *value = !*value;
        }

        const bool hovered = ImGui::IsItemHovered();
        auto *dl = ImGui::GetWindowDrawList();
        ImVec4 bg = Theme::Bg3();
        if (*value)
            bg = Theme::Accent();
        else if (hovered)
            bg = Theme::Hover();
        dl->AddRectFilled(pos, {pos.x + scaledSize.x, pos.y + scaledSize.y}, Theme::U32(bg), 10.0F * scale);
        dl->AddRect(pos, {pos.x + scaledSize.x, pos.y + scaledSize.y}, Theme::U32(*value ? Theme::Accent() : Theme::Border()),
                    10.0F * scale);
        const float knobX = *value ? pos.x + scaledSize.x - 15.0F * scale : pos.x + 3.0F * scale;
        dl->AddCircleFilled({knobX + 6.0F * scale, pos.y + 10.0F * scale}, 6.0F * scale,
                            Theme::U32(*value ? ImVec4{1, 1, 1, 1} : Theme::Dim()), 16);

        if (showLabel) {
            ImGui::SameLine(0.0F, 10.0F);
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
            ImGui::TextUnformatted(*value ? "Enabled" : "Disabled");
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
        return clicked;
    }

    bool MultiSelectField(const char *id, const char *label, const std::span<const char *const> items, const std::span<bool> values,
                          const Theme::Fonts &fonts, const float width, const ComponentSize componentSize) {
        std::size_t selectedCount = 0;
        for (const bool value : values)
            selectedCount += value ? 1U : 0U;
        std::string summary = selectedCount == 0U ? "None" : std::string{label};
        if (selectedCount > 0U)
            summary = std::format("{} ({})", summary, selectedCount);

        const auto &tokens = Theme::GetActiveTokens();
        const auto &metrics = DesignSystem::MetricsFor(tokens, componentSize);
        const float renderedTextHeight =
            fonts.sansCompact != nullptr ? fonts.sansCompact->FontSize * metrics.fontSize / Theme::FontPx::SansCompact : metrics.fontSize;
        const ImVec2 framePadding = DefaultFramePadding(metrics, renderedTextHeight);
        const float labelWidth = ImGui::CalcTextSize(label).x + framePadding.x * 2.0F + metrics.iconSize;
        const float resolvedWidth = width > 0.0F ? ScaledLayoutValue(width) : labelWidth;
        ImGui::SetNextItemWidth(resolvedWidth);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, framePadding);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
        Theme::ScopedTextStyle ts(fonts.sansCompact, metrics.fontSize, Theme::FontPx::SansCompact);

        const bool opened = ImGui::BeginCombo(id, summary.c_str());
        bool changed = false;
        if (opened) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, Theme::Bg2());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{8.0F, 6.0F});
            for (std::size_t index = 0; index < items.size() && index < values.size(); ++index) {
                bool value = values[index];
                if (CheckboxControl(items[index], &value, fonts)) {
                    values[index] = value;
                    changed = true;
                }
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            ImGui::EndCombo();
        }
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(2);
        return changed;
    }

    // ── CheckboxControl ──────────────────────────────────────────────────

    [[nodiscard]] bool CheckboxControl(const char *label, bool *value, const Theme::Fonts &fonts) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2{8.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
        PushControlColors(Theme::Bg3(), Theme::Border(), Theme::Muted());
        ImGui::PushStyleColor(ImGuiCol_CheckMark, Theme::Accent());

        bool clicked = false;
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            clicked = ImGui::Checkbox(label, value);
        }

        ImGui::PopStyleColor(6);
        ImGui::PopStyleVar(4);
        return clicked;
    }

    // ── PluginRow ────────────────────────────────────────────────────────

    void DrawPluginRowContent(const char *version, const char *description, bool *enabled, const Theme::Fonts &fonts) {
        const float cursorY = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(cursorY + 4.0F);
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
            ImGui::TextUnformatted(version);
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 1.0F);
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + Theme::Layout::ControlW - 52.0F);
            ImGui::TextWrapped("%s", description);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorPos({Theme::Layout::ControlW - 42.0F, cursorY + 6.0F});
        (void)ToggleControl("plugin-toggle", enabled, fonts, false);
    }

    void PluginRow(const char *name, const char *version, const char *description, bool *enabled, const Theme::Fonts &fonts) {
        SettingRow(name, nullptr, fonts, [version, description, enabled, &fonts]() {
            DrawPluginRowContent(version, description, enabled, fonts);
        });
    }

    // ── ShortcutDisplay ──────────────────────────────────────────────────

    void ShortcutDisplay(const char *a, const char *b, const char *c, const Theme::Fonts &fonts) {
        const std::array<const char *, 3> keys = {a, b, c};
        for (int i = 0; i < 3; ++i) {
            if (keys[i] == nullptr || keys[i][0] == '\0')
                continue;
            if (i > 0) {
                ImGui::SameLine(0.0F, 4.0F);
                {
                    Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::Dim());
                    ImGui::TextUnformatted("+");
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine(0.0F, 4.0F);
            }
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{7.0F, 3.0F});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
            ImGui::PushStyleColor(ImGuiCol_Button, Theme::Bg3());
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Bg3());
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Bg3());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            {
                Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                ImGui::Button(keys[i], ImVec2{0.0F, 24.0F});
            }
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
    }

    // ── ShortcutRecorder ──────────────────────────────────────────────────

    [[nodiscard]] bool StoreShortcut(ImGuiKey key, const ImGuiIO &io, std::string &keysOut) {
        std::string combo;
        if (io.KeyCtrl || io.KeySuper)
            combo += "Ctrl+";
        if (io.KeyShift)
            combo += "Shift+";
        if (io.KeyAlt)
            combo += "Alt+";
        combo += ImGui::GetKeyName(key);
        keysOut = std::move(combo);
        return true;
    }

    [[nodiscard]] bool PollShortcutInput(bool *listening, std::string &keysOut) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            *listening = false;
            return false;
        }
        const auto &io = ImGui::GetIO();
        for (int key = ImGuiKey_A; key <= ImGuiKey_Z; ++key)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) {
                *listening = false;
                return StoreShortcut(static_cast<ImGuiKey>(key), io, keysOut);
            }
        for (int key = ImGuiKey_F1; key <= ImGuiKey_F12; ++key)
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(key), false)) {
                *listening = false;
                return StoreShortcut(static_cast<ImGuiKey>(key), io, keysOut);
            }
        static constexpr std::array<ImGuiKey, 13> specialKeys = {ImGuiKey_Space,   ImGuiKey_Tab,       ImGuiKey_Backspace,
                                                                 ImGuiKey_Delete,  ImGuiKey_Enter,     ImGuiKey_Home,
                                                                 ImGuiKey_End,     ImGuiKey_LeftArrow, ImGuiKey_RightArrow,
                                                                 ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_PageUp,
                                                                 ImGuiKey_PageDown};
        for (const auto key : specialKeys)
            if (ImGui::IsKeyPressed(key, false)) {
                *listening = false;
                return StoreShortcut(key, io, keysOut);
            }
        return false;
    }

    struct ShortcutRecorderText {
        const char *placeholder;
        const char *listening;
    };

    void DrawShortcutContent(ImDrawList *drawList, const Theme::Fonts &fonts, const bool listening, const char *keysLabel,
                             const ImVec2 cursor, const ImVec2 size, const ShortcutRecorderText text) {
        using namespace Theme;
        if (listening) {
            const ImVec2 textSize = ImGui::CalcTextSize(text.listening);
            drawList->AddText(fonts.sansCompact, TextPx::Caption(),
                              {cursor.x + (size.x - textSize.x) * 0.5F, cursor.y + (size.y - textSize.y) * 0.5F}, U32(Accent()),
                              text.listening);
            return;
        }
        if (keysLabel == nullptr || keysLabel[0] == '\0') {
            const ImVec2 textSize = ImGui::CalcTextSize(text.placeholder);
            drawList->AddText(fonts.sansCompact, TextPx::Caption(),
                              {cursor.x + (size.x - textSize.x) * 0.5F, cursor.y + (size.y - textSize.y) * 0.5F}, U32(Dim()),
                              text.placeholder);
            return;
        }

        const std::string label{keysLabel};
        std::vector<std::string> parts;
        std::size_t pos = 0;
        while (pos < label.size()) {
            const auto next = label.find('+', pos);
            parts.push_back(label.substr(pos, next - pos));
            if (next == std::string::npos)
                break;
            pos = next + 1;
        }
        float totalWidth = 0.0F;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            totalWidth += ImGui::CalcTextSize(parts[index].c_str()).x + 10.0F;
            if (index + 1 < parts.size())
                totalWidth += 8.0F;
        }

        float x = cursor.x + (size.x - totalWidth) * 0.5F;
        const float y = cursor.y + (size.y - 14.0F) * 0.5F;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const auto &part = parts[index];
            const ImVec2 textSize = ImGui::CalcTextSize(part.c_str());
            const ImVec2 chipMin{x, y};
            const ImVec2 chipMax{x + textSize.x + 10.0F, y + 18.0F};
            drawList->AddRectFilled(chipMin, chipMax, U32(Bg3()), 3.0F);
            drawList->AddRect(chipMin, chipMax, U32(BorderStrong()), 3.0F, 0, 1.0F);
            drawList->AddText(fonts.sansCompact, TextPx::Caption(), {x + 5.0F, y + 2.0F}, U32(Text()), part.c_str());
            x += textSize.x + 10.0F;
            if (index + 1 < parts.size()) {
                drawList->AddText(fonts.sansCompact, TextPx::Caption(), {x + 2.0F, y + 2.0F}, U32(Dim()), "+");
                x += 12.0F;
            }
        }
    }

    [[nodiscard]] bool ShortcutRecorder(const char *id, const char *keysLabel, bool *listening, std::string &keysOut,
                                        const Theme::Fonts &fonts, const char *placeholderText, const char *listeningText) {
        using namespace Theme;
        ImGui::PushID(id);

        bool recorded = false;

        if (*listening)
            recorded = PollShortcutInput(listening, keysOut);

        // ── Draw the recorder UI ────────────────────────────────────────
        constexpr float width = Layout::ControlW;
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 size = {width, 28.0F};

        // Dashed border background
        auto *dl = ImGui::GetWindowDrawList();
        const ImU32 borderCol = *listening ? U32(Accent()) : U32(BorderStrong());
        const ImU32 bgCol =
            *listening ? ImGui::GetColorU32(ImVec4{Accent().x, Accent().y, Accent().z, 0.06F}) : U32(ImVec4{0.0F, 0.0F, 0.0F, 0.0F});

        dl->AddRectFilled(cursor, {cursor.x + size.x, cursor.y + size.y}, bgCol, GetActiveTokens().radii.control);

        // Draw dashed border manually (4px dash segments)
        const ImU32 dashCol = borderCol;
        const float r = GetActiveTokens().radii.control;
        const auto drawDashRect = [dl, dashCol, r](ImVec2 p0, ImVec2 p1) {
            // Simple solid border for now — dashed is complex in ImDrawList
            dl->AddRect(p0, p1, dashCol, r, 0, 1.0F);
        };
        drawDashRect(cursor, {cursor.x + size.x, cursor.y + size.y});

        // Invisible button for click detection
        ImGui::SetCursorScreenPos(cursor);
        ImGui::InvisibleButton("recorder", size);

        if (ImGui::IsItemClicked() && !(*listening)) {
            *listening = true;
        }

        DrawShortcutContent(dl, fonts, *listening, keysLabel, cursor, size, ShortcutRecorderText{placeholderText, listeningText});

        ImGui::PopID();
        return recorded;
    }

    // ── ThemeChip ────────────────────────────────────────────────────────

    [[nodiscard]] bool ThemeChip(const char *label, const ImVec4 swatch, const bool active, const Theme::Fonts &fonts) {
        ImGui::PushID(label);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{12.0F, 7.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);

        const auto accentGlow = ImVec4{Theme::Accent().x, Theme::Accent().y, Theme::Accent().z, 0.14F};
        ImGui::PushStyleColor(ImGuiCol_Button, active ? accentGlow : Theme::Bg3());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Hover());
        ImGui::PushStyleColor(ImGuiCol_Text, active ? Theme::Text() : Theme::Muted());
        ImGui::PushStyleColor(ImGuiCol_Border, active ? Theme::Accent() : Theme::Border());

        bool clicked = false;
        {
            Theme::ScopedTextStyle ts(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            clicked = ImGui::Button(label, ImVec2{82.0F, 32.0F});
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddCircleFilled({min.x + 12.0F, min.y + 16.0F}, 5.0F, Theme::U32(swatch), 16);
        ImGui::PopStyleColor(5);
        ImGui::PopStyleVar(3);
        ImGui::PopID();
        return clicked;
    }

    // ── Modal shell ──────────────────────────────────────────────────────

    /** @copydoc ScopedModalShell::ScopedModalShell */
    ScopedModalShell::ScopedModalShell(const ModalShellProps &props, const Theme::Fonts &fonts)
        : footerHeight_(ScaledLayoutValue(props.footerHeight)) {
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float width =
            std::min(ScaledLayoutValue(props.requestedSize.x),
                     std::max(ScaledLayoutValue(props.minimumWidth), viewport->WorkSize.x - ScaledLayoutValue(props.viewportPadding)));
        const float height =
            std::min(ScaledLayoutValue(props.requestedSize.y),
                     std::max(ScaledLayoutValue(props.minimumHeight), viewport->WorkSize.y - ScaledLayoutValue(props.viewportPadding)));
        const ImVec2 position{
            viewport->WorkPos.x + (viewport->WorkSize.x - width) * 0.5F,
            viewport->WorkPos.y + (viewport->WorkSize.y - height) * 0.5F,
        };

        ImGui::SetNextWindowPos(position, ImGuiCond_Appearing);
        ImGui::SetNextWindowSize({width, height}, ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Theme::GetActiveTokens().radii.modal);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg1());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::Begin(props.id, nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const ImVec2 modalPosition = ImGui::GetWindowPos();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg0());
        const float headerHeight = ScaledLayoutValue(props.headerHeight);
        ImGui::BeginChild("##ModalHeader", {0.0F, headerHeight}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        constexpr float iconSize = 20.0F;
        const float titleY = (headerHeight - ImGui::GetTextLineHeight()) * 0.5F;
        ImGui::SetCursorPosY(titleY);
        if (props.logo != 0) {
            ImGui::Image(props.logo, {iconSize, iconSize});
            ImGui::SameLine(0.0F, 10.0F);
        } else if (props.showBrandMark) {
            const ImVec2 mark = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled({mark.x + 4.0F, mark.y + 4.0F}, {mark.x + 14.0F, mark.y + 14.0F},
                                                      Theme::U32(Theme::Accent()), 2.0F);
            ImGui::Dummy({iconSize, iconSize});
            ImGui::SameLine(0.0F, 10.0F);
        }
        {
            Theme::ScopedTextStyle titleStyle(fonts.sansEmphasis, props.titleFontSize, Theme::FontPx::SansEmphasis);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            ImGui::TextUnformatted(props.title);
            ImGui::PopStyleColor();
        }

        bool closeHovered = false;
        if (props.showClose) {
            constexpr ImVec2 closeSize{28.0F, 28.0F};
            ImGui::SetCursorPos({ImGui::GetWindowWidth() - 50.0F, (props.headerHeight - closeSize.y) * 0.5F});
            closeRequested_ = IconCloseButton("##ModalClose", closeSize);
            closeHovered = ImGui::IsItemHovered();
        }

        const ImVec2 headerPosition = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine({headerPosition.x, headerPosition.y + props.headerHeight - 1.0F},
                                            {headerPosition.x + ImGui::GetWindowWidth(), headerPosition.y + props.headerHeight - 1.0F},
                                            Theme::U32(Theme::Border()), 1.0F);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        const ImVec2 headerMax{modalPosition.x + width, modalPosition.y + headerHeight};
        ImGuiStorage *const windowStorage = ImGui::GetStateStorage();
        const ImGuiID draggingStateId = ImGui::GetID("##ModalHeaderDragging");
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsMouseHoveringRect(modalPosition, headerMax, false) && !closeHovered) {
            windowStorage->SetBool(draggingStateId, true);
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
            windowStorage->SetBool(draggingStateId, false);
        if (windowStorage->GetBool(draggingStateId)) {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            const float maximumX = viewport->WorkPos.x + viewport->WorkSize.x - width;
            const float maximumY = viewport->WorkPos.y + viewport->WorkSize.y - height;
            const ImVec2 draggedPosition{
                std::clamp(modalPosition.x + delta.x, viewport->WorkPos.x, maximumX),
                std::clamp(modalPosition.y + delta.y, viewport->WorkPos.y, maximumY),
            };
            ImGui::SetWindowPos(draggedPosition, ImGuiCond_Always);
        }

        bodyHeight_ = std::max(0.0F, ImGui::GetWindowHeight() - headerHeight - footerHeight_);
        footerStartY_ = ImGui::GetWindowContentRegionMax().y - footerHeight_;
        // Region placement is explicit, but descendants retain the active theme's
        // normal ItemSpacing. Modal chrome must not collapse form/list spacing.
        ImGui::SetCursorPos({0.0F, headerHeight});
    }

    /** @copydoc ScopedModalShell::~ScopedModalShell */
    ScopedModalShell::~ScopedModalShell() {
        if (footerOpen_)
            EndFooter();
        ImGui::End();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(3);
    }

    /** @copydoc ScopedModalShell::BodyHeight */
    float ScopedModalShell::BodyHeight() const noexcept {
        return bodyHeight_;
    }

    /** @copydoc ScopedModalShell::FooterStartY */
    float ScopedModalShell::FooterStartY() const noexcept {
        return footerStartY_;
    }

    /** @copydoc ScopedModalShell::CloseRequested */
    bool ScopedModalShell::CloseRequested() const noexcept {
        return closeRequested_;
    }

    /** @copydoc ScopedModalShell::BeginFooter */
    void ScopedModalShell::BeginFooter(const ImVec2 padding, const bool border) {
        IM_ASSERT(!footerOpen_);
        ImGui::SetCursorPosY(footerStartY_);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg0());
        ImGui::BeginChild("##ModalFooter", {0.0F, footerHeight_}, border,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        const ImVec2 position = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(position, {position.x + ImGui::GetWindowWidth(), position.y}, Theme::U32(Theme::Border()),
                                            1.0F);
        footerOpen_ = true;
    }

    /** @copydoc ScopedModalShell::EndFooter */
    void ScopedModalShell::EndFooter() {
        IM_ASSERT(footerOpen_);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        footerOpen_ = false;
    }

    // ── Dock UI ───────────────────────────────────────────────────────────

    int DrawDockTabs(const std::span<const char *const> tabs, int activeTab, const Theme::Fonts &fonts, const float height,
                     const DockTabStyle style) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;

        const float tabH = std::max(1.0F, height);

        // Background (--bg-darkest)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + tabH), Theme::U32(Theme::Bg0()));
        // Bottom border (--border)
        const bool globalDockStyle = style == DockTabStyle::GlobalDock;
        dl->AddLine(ImVec2(pos.x, pos.y + tabH - 1.0f), ImVec2(pos.x + w, pos.y + tabH - 1.0f),
                    Theme::U32(globalDockStyle ? Theme::BorderStrong() : Theme::Border()), 1.0f);

        int clickedTab = -1;

        ImGui::SetCursorScreenPos(pos);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

        for (size_t i = 0; i < tabs.size(); ++i) {
            const bool isActive = (static_cast<int>(i) == activeTab);

            const float tabFontSize = Theme::TextPx::Label();
            const ImVec2 textSize = fonts.sans->CalcTextSizeA(tabFontSize, FLT_MAX, 0.0F, tabs[i]);
            auto buttonSize = ImVec2(textSize.x + 26.0f, tabH);  // padding 13px * 2

            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(tabs[i], buttonSize)) {
                clickedTab = static_cast<int>(i);
            }

            const bool hovered = ImGui::IsItemHovered();
            ImVec4 textColor = globalDockStyle ? Theme::Dim() : Theme::Muted();
            if (isActive)
                textColor = Theme::Text();
            else if (hovered)
                textColor = globalDockStyle ? Theme::Muted() : Theme::Dim();

            // Draw text
            const float textY = p.y + (tabH - tabFontSize) * 0.5F;
            dl->AddText(fonts.sans, tabFontSize, ImVec2(p.x + 13.0f, textY), ImGui::GetColorU32(textColor), tabs[i]);

            // Active underline
            if (isActive) {
                dl->AddLine(ImVec2(p.x, p.y + tabH - 1.5f), ImVec2(p.x + buttonSize.x, p.y + tabH - 1.5f), Theme::U32(Theme::Accent()),
                            2.0f);
            }

            ImGui::SameLine();
        }

        ImGui::PopStyleVar();
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + tabH));

        return clickedTab != -1 ? clickedTab : activeTab;
    }

    /** @copydoc DrawSideDockTabs */
    int DrawSideDockTabs(const std::span<const char *const> tabs, int activeTab, const Theme::Fonts &fonts) {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float tabHeight = 36.0F * scale;
        const float dockPadding = 10.0F * scale;
        const float horizontalPadding = 10.0F * scale;
        const float fontSize = Theme::TextPx::Label();
        const float underlineHeight = std::max(1.0F, 2.0F * scale);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList &drawList = *ImGui::GetWindowDrawList();

        drawList.AddRectFilled(origin, {origin.x + width, origin.y + tabHeight}, Theme::U32(Theme::Bg0()));
        drawList.AddLine({origin.x, origin.y + tabHeight - scale}, {origin.x + width, origin.y + tabHeight - scale},
                         Theme::U32(Theme::Border()), std::max(1.0F, scale));

        int clickedTab = -1;
        float tabX = origin.x + dockPadding;
        ImFont *font = fonts.sans != nullptr ? fonts.sans : ImGui::GetFont();
        for (std::size_t index = 0; index < tabs.size(); ++index) {
            const bool active = static_cast<int>(index) == activeTab;
            const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, tabs[index]);
            const float tabWidth = textSize.x + horizontalPadding * 2.0F;
            const ImVec2 tabPosition{tabX, origin.y};

            ImGui::SetCursorScreenPos(tabPosition);
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::InvisibleButton("##side_dock_tab", {tabWidth, tabHeight}))
                clickedTab = static_cast<int>(index);
            ImGui::PopID();

            const bool hovered = ImGui::IsItemHovered();
            ImVec4 textColor = Theme::Muted();
            if (active)
                textColor = Theme::Text();
            else if (hovered)
                textColor = Theme::Dim();
            drawList.AddText(font, fontSize, {tabX + horizontalPadding, origin.y + (tabHeight - textSize.y) * 0.5F}, Theme::U32(textColor),
                             tabs[index]);
            if (active) {
                drawList.AddRectFilled({tabX, origin.y + tabHeight - underlineHeight}, {tabX + tabWidth, origin.y + tabHeight},
                                       Theme::U32(Theme::Accent()));
            }
            tabX += tabWidth;
            ImGui::SameLine(0.0F, 0.0F);
        }

        ImGui::SetCursorScreenPos({origin.x, origin.y + tabHeight});
        return clickedTab != -1 ? clickedTab : activeTab;
    }

    void DrawObjTitle(const char *title, const char *badgeText, ImVec4 badgeBg, ImVec4 badgeFg, const Theme::Fonts &fonts) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        constexpr float h = 38.0f;

        // border-bottom
        dl->AddLine(ImVec2(pos.x, pos.y + h - 1.0f), ImVec2(pos.x + w, pos.y + h - 1.0f), Theme::U32(Theme::Border()), 1.0f);

        const float titleFontSize = Theme::TextPx::Title();
        dl->AddText(fonts.sans, titleFontSize, ImVec2(pos.x + 14.0f, pos.y + (h - titleFontSize) * 0.5F), Theme::U32(Theme::Text()), title);

        // Badge
        const float badgeFontSize = Theme::TextPx::Caption();
        ImVec2 badgeSize = fonts.sansCompact->CalcTextSizeA(badgeFontSize, FLT_MAX, 0.0F, badgeText);
        badgeSize.x += 12.0f;  // padding 6px
        badgeSize.y += 6.0f;   // padding 3px

        const auto badgePos = ImVec2(pos.x + w - 14.0f - badgeSize.x, pos.y + 10.0f);
        dl->AddRectFilled(badgePos, ImVec2(badgePos.x + badgeSize.x, badgePos.y + badgeSize.y), ImGui::GetColorU32(badgeBg), 4.0f);
        dl->AddText(fonts.sansCompact, badgeFontSize, ImVec2(badgePos.x + 6.0f, badgePos.y + 3.0f), ImGui::GetColorU32(badgeFg), badgeText);

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    }

    /** @copydoc DrawEditableTitle */
    TextEditResult DrawEditableTitle(const char *id, std::string &value, const size_t maximumBytes, const Theme::Fonts &fonts,
                                     const EditableTitleProps &props) {
        if (maximumBytes == 0)
            return {};

        const EditableTitleLayout layout = ResolveEditableTitleLayout(props, fonts);
        DrawEditorIcon(ImGui::GetWindowDrawList(), props.leadingIcon,
                       {layout.position.x, layout.position.y + (layout.height - layout.iconSize) * 0.5F},
                       {layout.iconSize, layout.iconSize}, Theme::U32(Theme::Accent()), fonts.icon);

        value.resize(std::min(value.size(), maximumBytes));
        value.resize(maximumBytes, '\0');
        const TextEditResult result = DrawEditableTitleInput(id, value, fonts, props, layout);

        const auto nullPosition = value.find('\0');
        value.resize(nullPosition == std::string::npos ? value.size() : nullPosition);

        ImGui::SetCursorScreenPos({layout.position.x, layout.position.y + layout.height});
        return result;
    }

    /** @copydoc Card::Card */
    Card::Card(const CardProps &props) {
        ImGui::PushID(props.id);
        expandedStateId_ = ImGui::GetID("##expanded");
        ImGui::PopID();
        stateStorage_ = ImGui::GetStateStorage();
        expanded_ = stateStorage_->GetBool(expandedStateId_, props.defaultExpanded);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 5.0F);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::CardSurface());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::CardBorder());
        ImGui::BeginChild(props.id, {0.0F, 0.0F}, ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                          ImGuiWindowFlags_NoScrollbar);
    }

    /** @copydoc Card::~Card */
    Card::~Card() {
        Finish();
    }

    namespace {
        /** @brief Draws and dispatches one card action popup menu. */
        void DrawCardActionMenu(const CardTitleBarAction &action, const Theme::Fonts &fonts) {
            if (action.menuItems.empty() || !BeginMenuPopup("##card_action_menu"))
                return;
            for (const CardMenuAction &menuItem : action.menuItems) {
                if (menuItem.separatorBefore)
                    ContextMenuSeparator();
                ImGui::PushID(menuItem.id);
                if (const bool selected = ContextMenuItem(menuItem.label, nullptr, fonts,
                                                          menuItem.destructive ? ContextMenuItemTone::Danger : ContextMenuItemTone::Normal,
                                                          UiIconRegistry::Token(menuItem.icon), menuItem.enabled);
                    selected && menuItem.onInvoke)
                    menuItem.onInvoke();
                ImGui::PopID();
            }
            EndMenuPopup();
        }

        /** @brief Interaction state produced by one compact card-title action surface. */
        struct CardTitleActionInteraction {
            bool pressed{false};
        };

        /** @brief Draws one compact card-title action surface. */
        [[nodiscard]] CardTitleActionInteraction DrawCardTitleActionSurface(const CardTitleBarAction &action, const ImVec2 titlePosition,
                                                                            const float x, ImDrawList &drawList,
                                                                            const Theme::Fonts &fonts) {
            constexpr float toolSize = 24.0F;
            ImGui::SetCursorScreenPos({x, titlePosition.y + 5.0F});
            ImGui::PushID(action.id);
            ImGui::BeginDisabled(!action.enabled);
            const bool pressed = ImGui::InvisibleButton("##card_action", {toolSize, toolSize});
            const bool hovered = action.enabled && ImGui::IsItemHovered();
            if (hovered)
                drawList.AddRectFilled({x, titlePosition.y + 5.0F}, {x + toolSize, titlePosition.y + 5.0F + toolSize},
                                       Theme::U32(Theme::Hover()), 3.0F);
            ImVec4 resolvedColor = action.active ? Theme::Accent() : Theme::Muted();
            if (hovered)
                resolvedColor = Theme::Text();
            if (!action.enabled)
                resolvedColor.w *= 0.45F;
            DrawEditorIcon(&drawList, action.icon, {x + 4.0F, titlePosition.y + 9.0F}, {16.0F, 16.0F}, Theme::U32(resolvedColor),
                           fonts.icon);
            if (hovered && action.title != nullptr && action.title[0] != '\0')
                ShowTooltip(action.title, &fonts);
            ImGui::EndDisabled();
            return {.pressed = pressed};
        }

        /** @brief Dispatches one card-title interaction and its optional popup menu. */
        void DispatchCardTitleAction(const CardTitleBarAction &action, const bool pressed, const Theme::Fonts &fonts) {
            if (pressed && action.menuItems.empty() && action.onInvoke)
                action.onInvoke();
            if (pressed && !action.menuItems.empty())
                ImGui::OpenPopup("##card_action_menu");
            DrawCardActionMenu(action, fonts);
        }

        /** @brief Draws and dispatches one compact action in a card title bar. */
        void DrawCardTitleAction(const CardTitleBarAction &action, const ImVec2 titlePosition, const float x, ImDrawList &drawList,
                                 const Theme::Fonts &fonts) {
            const CardTitleActionInteraction interaction = DrawCardTitleActionSurface(action, titlePosition, x, drawList, fonts);
            DispatchCardTitleAction(action, interaction.pressed, fonts);
            ImGui::PopID();
        }
    }  // namespace

    /** @copydoc Card::DrawTitleBar */
    void Card::DrawTitleBar(const CardTitleBarProps &props) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        constexpr float h = 34.0F;

        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), Theme::U32(Theme::CardHeaderSurface()));
        dl->AddLine(ImVec2(pos.x, pos.y + h - 1.0F), ImVec2(pos.x + w, pos.y + h - 1.0F), Theme::U32(Theme::CardBorder()), 1.0F);

        const ImVec2 chevronCenter{pos.x + 12.0F, pos.y + h * 0.5F};
        if (expanded_)
            dl->AddTriangleFilled({chevronCenter.x - 3.0F, chevronCenter.y - 2.0F}, {chevronCenter.x + 3.0F, chevronCenter.y - 2.0F},
                                  {chevronCenter.x, chevronCenter.y + 2.0F}, Theme::U32(Theme::Muted()));
        else
            dl->AddTriangleFilled({chevronCenter.x - 2.0F, chevronCenter.y - 3.0F}, {chevronCenter.x - 2.0F, chevronCenter.y + 3.0F},
                                  {chevronCenter.x + 2.0F, chevronCenter.y}, Theme::U32(Theme::Muted()));
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(props.id);
        if (ImGui::InvisibleButton("##card_disclosure", {24.0F, h})) {
            expanded_ = !expanded_;
            stateStorage_->SetBool(expandedStateId_, expanded_);
        }
        ImGui::PopID();

        const float headerFontSize = CardTypography::Title();
        ImFont *titleFont = props.fonts.sansEmphasis != nullptr ? props.fonts.sansEmphasis : ImGui::GetFont();
        dl->AddText(titleFont, headerFontSize, ImVec2(pos.x + 28.0F, pos.y + (h - headerFontSize) * 0.5F), Theme::U32(Theme::Text()),
                    props.title);

        constexpr float toolSize = 24.0F;
        constexpr float toolGap = 2.0F;
        const float toolsWidth = props.actions.empty() ? 0.0F
                                                       : toolSize * static_cast<float>(props.actions.size()) +
                                                             toolGap * static_cast<float>(props.actions.size() - 1U);
        const float toolsX = pos.x + w - toolsWidth - 4.0F;
        ImGui::PushID(props.id);
        for (std::size_t index = 0; index < props.actions.size(); ++index) {
            const float x = toolsX + static_cast<float>(index) * (toolSize + toolGap);
            DrawCardTitleAction(props.actions[index], pos, x, *dl, props.fonts);
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    }

    /** @copydoc Card::BeginBody */
    bool Card::BeginBody() {
        if (!expanded_)
            return false;
        if (bodyOpen_)
            return true;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7.0F);
        bodyOpen_ = true;
        return true;
    }

    /** @copydoc Card::Finish */
    void Card::Finish() {
        if (!open_)
            return;
        if (bodyOpen_)
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7.0F);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        open_ = false;
    }

    void DrawPropRow(const char *label, const char *value, const Theme::Fonts &fonts) {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        constexpr float h = 28.0f;

        ImVec4 borderLight = Theme::Border();
        borderLight.w = 0.5f;
        dl->AddLine(ImVec2(pos.x, pos.y + h - 1.0f), ImVec2(pos.x + w, pos.y + h - 1.0f), ImGui::GetColorU32(borderLight), 1.0f);

        const float labelFontSize = InspectorTypography::Label();
        const float fieldFontSize = InspectorTypography::Field();
        dl->AddText(fonts.sans, labelFontSize, ImVec2(pos.x + 14.0f, pos.y + (h - labelFontSize) * 0.5F), Theme::U32(Theme::Muted()),
                    label);

        constexpr float labelW = 80.0f;
        const auto inputPos = ImVec2(pos.x + 14.0f + labelW, pos.y + 4.0f);
        const auto inputSize = ImVec2(w - 28.0f - labelW, h - 8.0f);

        dl->AddRectFilled(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y), Theme::U32(Theme::InspectorFieldSurface()),
                          4.0f);
        dl->AddRect(inputPos, ImVec2(inputPos.x + inputSize.x, inputPos.y + inputSize.y), Theme::U32(Theme::InspectorBorder()), 4.0f);

        dl->AddText(fonts.sans, fieldFontSize, ImVec2(inputPos.x + 8.0f, inputPos.y + (inputSize.y - fieldFontSize) * 0.5F),
                    Theme::U32(Theme::Text()), value);

        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    }

    /** @copydoc BeginContextMenu */
    bool BeginContextMenu(const char *id) {
        SetNextContextMenuSizeConstraints();
        PushContextPopupWindowStyle();
        const bool open = ImGui::BeginPopupContextItem(id, ImGuiPopupFlags_MouseButtonRight);
        if (!open)
            PopContextPopupWindowStyle();
        return open;
    }

    /** @copydoc BeginContextWindowMenu */
    bool BeginContextWindowMenu(const char *id) {
        SetNextContextMenuSizeConstraints();
        PushContextPopupWindowStyle();
        const bool open = ImGui::BeginPopupContextWindow(id, ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems);
        if (!open)
            PopContextPopupWindowStyle();
        return open;
    }

    /** @copydoc EndContextMenu */
    void EndContextMenu() {
        ImGui::EndPopup();
        PopContextPopupWindowStyle();
    }

    /** @copydoc BeginMenuPopup */
    bool BeginMenuPopup(const char *id) {
        SetNextContextMenuSizeConstraints();
        PushContextPopupWindowStyle();
        const bool open = ImGui::BeginPopup(id);
        if (!open) {
            PopContextPopupWindowStyle();
        }
        return open;
    }

    /** @copydoc EndMenuPopup */
    void EndMenuPopup() {
        ImGui::EndPopup();
        PopContextPopupWindowStyle();
    }

    /** @copydoc BeginMenuDropdown */
    bool BeginMenuDropdown(const char *label, const Theme::Fonts &fonts) {
        SetNextContextMenuSizeConstraints();
        PushMenuDropdownWindowStyle();
        bool open = false;
        {
            Theme::ScopedTextStyle textStyle(fonts.sans, Theme::TextPx::Label(), Theme::FontPx::Sans);
            open = ImGui::BeginMenu(label);
        }
        if (open)
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0.0F, 0.0F});
        else
            PopMenuDropdownWindowStyle();
        return open;
    }

    /** @copydoc EndMenuDropdown */
    void EndMenuDropdown() {
        ImGui::PopStyleVar();
        ImGui::EndMenu();
        PopMenuDropdownWindowStyle();
    }

    /** @copydoc ContextMenuItem */
    bool ContextMenuItem(const char *label, const char *shortcut, const Theme::Fonts &fonts, const ContextMenuItemTone tone,
                         const std::string_view iconToken, const bool enabled) {
        const char *stableId = std::strstr(label, "###");
        if (stableId == nullptr)
            ImGui::PushID(label);
        const char *rowId = stableId != nullptr ? stableId : "##item";
        const ContextMenuRow row = DrawContextMenuRow(rowId, false, false, enabled);
        if (stableId == nullptr)
            ImGui::PopID();
        ImVec4 textColor = tone == ContextMenuItemTone::Danger ? Theme::Err() : Theme::Text();
        if (!enabled)
            textColor = Theme::Dim();
        DrawContextMenuRowPresentation(row, {label, shortcut, fonts, textColor, row.hovered && enabled, false, iconToken});
        return enabled && row.activated;
    }

    /** @copydoc BeginContextSubmenu */
    bool BeginContextSubmenu(const char *label, const Theme::Fonts &fonts, const std::string_view iconToken) {
        const std::string popupId = std::string{"##submenu_popup_"} + label;
        const bool wasOpen = ImGui::IsPopupOpen(popupId.c_str());
        const char *stableId = std::strstr(label, "###");
        if (stableId == nullptr)
            ImGui::PushID(label);
        // An already-open child popup blocks normal item hover on its parent menu.
        // Allow hovering sibling rows so moving the pointer switches submenus.
        const ContextMenuRow row = DrawContextMenuRow(stableId != nullptr ? stableId : "##submenu", wasOpen, true, true,
                                                      ImGuiHoveredFlags_AllowWhenBlockedByPopup);
        if (stableId == nullptr)
            ImGui::PopID();
        DrawContextMenuRowPresentation(row, {label, nullptr, fonts, Theme::Text(), row.hovered || wasOpen, true, iconToken});
        if (row.hovered || row.activated)
            ImGui::OpenPopup(popupId.c_str());

        ImGui::SetNextWindowPos({ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - 1.0F, row.minimum.y - ScaledLayoutValue(4.0F)});
        SetNextContextMenuSizeConstraints();
        PushContextPopupWindowStyle();
        const bool open = ImGui::BeginPopup(popupId.c_str(), ImGuiWindowFlags_NoMove);
        if (!open)
            PopContextPopupWindowStyle();
        return open;
    }

    /** @copydoc EndContextSubmenu */
    void EndContextSubmenu() {
        ImGui::EndPopup();
        PopContextPopupWindowStyle();
    }

    /** @copydoc ContextMenuSeparator */
    void ContextMenuSeparator() {
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        constexpr float height = 9.0F;
        ImGui::GetWindowDrawList()->AddLine({position.x + 5.0F * scale, position.y + 4.0F * scale},
                                            {position.x + width - 5.0F * scale, position.y + 4.0F * scale}, Theme::U32(Theme::Border()),
                                            1.0F);
        ImGui::Dummy({0.0F, height * scale});
    }

    /** @copydoc DrawComboPropRow */
    bool DrawComboPropRow(const char *label, const char *id, int &value, const std::span<const char *const> entries,
                          const Theme::Fonts &fonts) {
        const PropertyRowLayout layout = BeginPropertyRow(label, fonts);
        ImGui::SetCursorScreenPos({layout.controlX, layout.controlY});
        ImGui::PushItemWidth(layout.controlWidth);
        const bool changed = ComboControl(id, &value, entries.data(), static_cast<int>(entries.size()), fonts,
                                          ComboControlOptions{.height = 27.0F, .componentSize = ComponentSize::Medium});
        ImGui::PopItemWidth();
        EndPropertyRow(layout);
        return changed;
    }

    /** @copydoc DrawFloatPropRow */
    PropertyEditResult DrawFloatPropRow(const char *label, const char *id, float &value, const Theme::Fonts &fonts,
                                        const FloatPropertyOptions options) {
        const PropertyRowLayout layout = BeginPropertyRow(label, fonts);
        ImGui::SetCursorScreenPos({layout.controlX, layout.controlY});
        ImGui::PushID(id);
        ImGui::PushItemWidth(layout.controlWidth);
        PushInspectorControlStyle();
        if (options.error)
            PushErrorControlColors();
        PropertyEditResult result;
        {
            Theme::ScopedTextStyle ts(fonts.sans, InspectorTypography::Field(), Theme::FontPx::Sans);
            result.changed = ImGui::DragFloat("##value", &value, options.speed, options.minimum, options.maximum, options.format);
            result.committed = ImGui::IsItemDeactivatedAfterEdit();
        }
        if (options.error)
            ImGui::PopStyleColor(4);
        PopControlStyle();
        ImGui::PopItemWidth();
        ImGui::PopID();
        EndPropertyRow(layout);
        return result;
    }

    /** @copydoc DrawColor3PropRow */
    PropertyEditResult DrawColor3PropRow(const char *label, const char *id, std::array<float, 3> &value, const Theme::Fonts &fonts,
                                         const bool error) {
        const PropertyRowLayout layout = BeginPropertyRow(label, fonts);
        ImGui::SetCursorScreenPos({layout.controlX, layout.controlY});
        ImGui::PushID(id);
        ImGui::PushItemWidth(layout.controlWidth);
        PushInspectorControlStyle();
        if (error)
            PushErrorControlColors();
        PropertyEditResult result;
        {
            Theme::ScopedTextStyle ts(fonts.sans, InspectorTypography::Field(), Theme::FontPx::Sans);
            constexpr ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_DisplayRGB |
                                                  ImGuiColorEditFlags_PickerHueWheel;
            result.changed = ImGui::ColorEdit3("##value", value.data(), flags);
            result.committed = ImGui::IsItemDeactivatedAfterEdit();
        }
        if (error)
            ImGui::PopStyleColor(4);
        PopControlStyle();
        ImGui::PopItemWidth();
        ImGui::PopID();
        EndPropertyRow(layout);
        return result;
    }

    /** @copydoc DrawFloat3PropRow */
    Float3PropertyEditResult DrawFloat3PropRow(const char *label, const char *id, std::array<float, 3> &value, const Theme::Fonts &fonts,
                                               const float speed, const std::array<bool, 3> &mixed) {
        const PropertyRowLayout layout = BeginPropertyRow(label, fonts);
        ImGui::SetCursorScreenPos({layout.controlX, layout.controlY});
        ImGui::PushID(id);
        PushInspectorControlStyle();
        Float3PropertyEditResult result;
        {
            const float valueFontSize = InspectorTypography::Field();
            const float axisFontSize = InspectorTypography::Axis();
            Theme::ScopedTextStyle ts(fonts.sans, valueFontSize, Theme::FontPx::Sans);
            constexpr std::array<const char *, 3> axisLabels{"X", "Y", "Z"};
            const std::array axisColors{Theme::Err(), Theme::Ok(), ImVec4{0.30F, 0.65F, 1.0F, 1.0F}};
            constexpr float axisGap = 5.0F;
            constexpr float axisLabelWidth = 12.0F;
            constexpr float axisLabelGap = 2.0F;
            const float axisWidth = std::max(1.0F, (layout.controlWidth - axisGap * 2.0F) / 3.0F);
            for (std::size_t axis = 0; axis < value.size(); ++axis) {
                if (axis != 0)
                    ImGui::SameLine(0.0F, axisGap);
                ImGui::PushID(static_cast<int>(axis));
                const ImVec2 axisPosition = ImGui::GetCursorScreenPos();
                const ImVec2 axisTextSize = fonts.sans->CalcTextSizeA(axisFontSize, FLT_MAX, 0.0F, axisLabels[axis]);
                ImGui::GetWindowDrawList()->AddText(fonts.sans, axisFontSize,
                                                    {axisPosition.x + (axisLabelWidth - axisTextSize.x) * 0.5F,
                                                     axisPosition.y + (ImGui::GetFrameHeight() - axisTextSize.y) * 0.5F},
                                                    ImGui::GetColorU32(axisColors[axis]), axisLabels[axis]);
                ImGui::SetCursorScreenPos({axisPosition.x + axisLabelWidth + axisLabelGap, axisPosition.y});
                ImGui::SetNextItemWidth(std::max(1.0F, axisWidth - axisLabelWidth - axisLabelGap));
                result.changedAxes[axis] = ImGui::DragFloat("##value", &value[axis], speed, 0.0F, 0.0F, "%.2f");
                result.changed = result.changed || result.changedAxes[axis];
                result.committed = result.committed || ImGui::IsItemDeactivatedAfterEdit();
                if (mixed[axis] && !result.changedAxes[axis] && !ImGui::IsItemActive()) {
                    const ImVec2 minimum = ImGui::GetItemRectMin();
                    const ImVec2 maximum = ImGui::GetItemRectMax();
                    ImDrawList *drawList = ImGui::GetWindowDrawList();
                    drawList->AddRectFilled(minimum, maximum, ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetStyle().FrameRounding);
                    drawList->AddRect(minimum, maximum, ImGui::GetColorU32(ImGuiCol_Border), ImGui::GetStyle().FrameRounding);
                    const ImVec2 textSize = ImGui::CalcTextSize("—");
                    drawList->AddText({minimum.x + (maximum.x - minimum.x - textSize.x) * 0.5F,
                                       minimum.y + (maximum.y - minimum.y - textSize.y) * 0.5F},
                                      ImGui::GetColorU32(Theme::Dim()), "—");
                }
                ImGui::PopID();
            }
        }
        PopControlStyle();
        ImGui::PopID();
        EndPropertyRow(layout);
        return result;
    }
}  // namespace Horo::Editor::Ui
