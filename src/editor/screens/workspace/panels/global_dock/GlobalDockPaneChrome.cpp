#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneChrome.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPaneLayout.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <ranges>
#include <string>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] ImFont *ResolveFont(ImFont *preferred) noexcept {
            return preferred != nullptr ? preferred : ImGui::GetFont();
        }

        [[nodiscard]] float TextWidth(ImFont *font, const float size, const std::string_view text) {
            if (text.empty())
                return 0.0F;
            return ResolveFont(font)->CalcTextSizeA(size, FLT_MAX, 0.0F, text.data(), text.data() + text.size()).x;
        }

        [[nodiscard]] ImVec4 ToolbarChipLabelColor(const GlobalDockToolbarChipProps &props) noexcept {
            if (props.disabled)
                return Theme::Dim();
            if (props.toneLabel)
                return GlobalDockToneColor(props.tone);
            return props.active ? Theme::Text() : Theme::Muted();
        }

        void DrawToolbarChipOutline(ImDrawList &drawList, const ImVec2 origin, const ImVec2 maximum,
                                    const GlobalDockToolbarChipProps &props, const float scale) {
            drawList.AddRect(origin, maximum, Theme::U32(props.active ? Theme::Accent() : Theme::Border()),
                             Theme::GetActiveTokens().radii.control);
            if (!props.active)
                return;
            const float inset = std::max(1.0F, scale);
            drawList.AddRect({origin.x + inset, origin.y + inset}, {maximum.x - inset, maximum.y - inset}, Theme::U32(Theme::AccentSoft()),
                             std::max(0.0F, Theme::GetActiveTokens().radii.control - inset));
        }

        [[nodiscard]] float DrawToolbarChipIcon(ImDrawList &drawList, const ImVec2 origin, const GlobalDockToolbarChipProps &props,
                                                const Theme::Fonts &fonts, const GlobalDockPaneMetrics &metrics, const float scale) {
            float x = origin.x + metrics.toolbarGap;
            if (props.icon == Ui::UiIcon::None)
                return x;
            const float iconSize = 14.0F * scale;
            Ui::DrawEditorIcon(&drawList, props.icon, {x, origin.y + (metrics.controlHeight - iconSize) * 0.5F}, {iconSize, iconSize},
                               Theme::U32(GlobalDockToneColor(props.tone)), fonts.icon);
            return x + iconSize + metrics.toolbarGap;
        }

        void DrawToolbarChipCount(ImDrawList &drawList, ImFont *font, const ImVec2 origin, const ImVec2 maximum,
                                  const GlobalDockToolbarChipProps &props, const GlobalDockPaneMetrics &metrics) {
            if (!props.count.has_value())
                return;
            const std::string count = std::to_string(*props.count);
            const float countSize = Theme::TextPx::Caption();
            const float countWidth = TextWidth(font, countSize, count);
            drawList.AddText(font, countSize,
                             {maximum.x - metrics.toolbarGap - countWidth, origin.y + (metrics.controlHeight - countSize) * 0.5F},
                             Theme::U32(GlobalDockToneColor(props.tone)), count.c_str());
        }
    }  // namespace

    ImVec4 GlobalDockToneColor(const GlobalDockTone tone) noexcept {
        switch (tone) {
            case GlobalDockTone::Accent:
                return Theme::Accent();
            case GlobalDockTone::Positive:
                return Theme::Ok();
            case GlobalDockTone::Warning:
                return Theme::Warn();
            case GlobalDockTone::Error:
                return Theme::Err();
            case GlobalDockTone::Neutral:
                return Theme::Muted();
        }
        return Theme::Muted();
    }

    void DrawGlobalDockToolbarSurface(const ImVec2 origin, const float width, const float height) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockToolbarSurface()));
        drawList->AddLine({origin.x, origin.y + height - 1.0F}, {origin.x + width, origin.y + height - 1.0F}, Theme::U32(Theme::Border()));
    }

    bool GlobalDockContainsCaseInsensitive(const std::string_view value, const std::string_view query) {
        if (query.empty())
            return true;
        return std::ranges::search(value, query, [](const char lhs, const char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) == std::tolower(static_cast<unsigned char>(rhs));
        }).begin() != value.end();
    }

    float MeasureGlobalDockTextWidth(ImFont *font, const float size, const std::string_view text) {
        return TextWidth(font, size, text);
    }

    /** @copydoc ResolveGlobalDockSearchWidth */
    float ResolveGlobalDockSearchWidth(const float toolbarWidth, const float trailingWidth) noexcept {
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        return std::max(1.0F, toolbarWidth - metrics.toolbarPaddingX * 2.0F - std::max(0.0F, trailingWidth));
    }

    float DrawGlobalDockSearchControl(const ImVec2 origin, const float width, const std::string_view id, const std::span<char> buffer,
                                      const std::string_view hint, const Theme::Fonts &fonts) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        ImGui::SetCursorScreenPos(origin);
        const std::string controlId{id};
        const std::string hintText{hint};
        static_cast<void>(Ui::InputTextControl(controlId.c_str(), buffer.data(), buffer.size(), fonts,
                                               {.width = width / scale,
                                                .hint = hintText.c_str(),
                                                .prefixIconWidth = 20.0F,
                                                .componentSize = Ui::ComponentSize::Small,
                                                .surface = Ui::InputTextSurface::BottomDockToolbar}));
        Ui::DrawEditorIcon(ImGui::GetWindowDrawList(), Ui::UiIcon::Search, {origin.x + 8.0F * scale, origin.y + 8.0F * scale},
                           {14.0F * scale, 14.0F * scale}, Theme::U32(Theme::Dim()), fonts.icon);
        return origin.x + width + ResolveGlobalDockPaneMetrics().toolbarGap;
    }

    void DrawGlobalDockTableHeaderSurface(const ImVec2 origin, const float width, const float height) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockContentSurface()));
        drawList->AddLine({origin.x, origin.y + height - 1.0F}, {origin.x + width, origin.y + height - 1.0F}, Theme::U32(Theme::Border()));
    }

    void DrawGlobalDockTableRowSurface(const ImVec2 origin, const float width, const float height, const bool hovered,
                                       const bool selected) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (selected)
            drawList->AddRectFilled(origin, {origin.x + width, origin.y + height},
                                    Theme::U32(Theme::Mix(Theme::Hover(), Theme::Accent(), 0.16F)));
        else if (hovered)
            drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::Hover()));
        drawList->AddLine({origin.x, origin.y + height - 1.0F}, {origin.x + width, origin.y + height - 1.0F},
                          Theme::U32(Theme::ConsoleRowBorder()));
    }

    void DrawGlobalDockMetricCard(const ImVec2 origin, const ImVec2 size, const GlobalDockMetricCardProps &props,
                                  const Theme::Fonts &fonts) {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float padding = 10.0F * scale;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 maximum{origin.x + size.x, origin.y + size.y};
        drawList->AddRectFilled(origin, maximum, Theme::U32(Theme::BottomDockMetricSurface()), Theme::GetActiveTokens().radii.card);
        drawList->AddRect(origin, maximum, Theme::U32(Theme::Border()), Theme::GetActiveTokens().radii.card);

        ImFont *labelFont = ResolveFont(fonts.sans);
        ImFont *valueFont = ResolveFont(fonts.sansCompact);
        const float labelSize = Theme::TextPx::Caption();
        const float valueSize = Theme::TextPx::CardTitle();
        drawList->AddText(labelFont, labelSize, {origin.x + padding, origin.y + padding}, Theme::U32(Theme::Muted()), props.label.data(),
                          props.label.data() + props.label.size());
        const float valueY = origin.y + padding + labelSize + 3.0F * scale;
        const ImVec4 valueColor = props.valueTone == GlobalDockTone::Neutral ? Theme::Text() : GlobalDockToneColor(props.valueTone);
        drawList->AddText(valueFont, valueSize, {origin.x + padding, valueY}, Theme::U32(valueColor), props.value.data(),
                          props.value.data() + props.value.size());
        if (!props.note.empty()) {
            const float noteX = origin.x + padding + TextWidth(valueFont, valueSize, props.value) + 4.0F * scale;
            const ImVec4 noteColor = props.noteTone == GlobalDockTone::Neutral ? Theme::Muted() : GlobalDockToneColor(props.noteTone);
            drawList->AddText(labelFont, labelSize, {noteX, valueY + valueSize - labelSize}, Theme::U32(noteColor), props.note.data(),
                              props.note.data() + props.note.size());
        }

        if (props.samples.size() < 2U)
            return;
        const float chartTop = valueY + valueSize + 5.0F * scale;
        const float chartBottom = maximum.y - padding;
        const float chartWidth = std::max(1.0F, size.x - padding * 2.0F);
        const float chartHeight = std::max(1.0F, chartBottom - chartTop);
        if (props.budget.has_value()) {
            const float budgetY = chartBottom - std::clamp(*props.budget, 0.0F, 1.0F) * chartHeight;
            const ImU32 budgetColor = Theme::U32(GlobalDockToneColor(GlobalDockTone::Warning));
            const auto dashCount = static_cast<std::size_t>(std::ceil(chartWidth / (6.0F * scale)));
            for (std::size_t dash = 0; dash < dashCount; ++dash) {
                const float dashX = origin.x + padding + static_cast<float>(dash) * 6.0F * scale;
                drawList->AddLine({dashX, budgetY}, {std::min(dashX + 3.0F * scale, maximum.x - padding), budgetY}, budgetColor);
            }
        }
        for (std::size_t index = 1; index < props.samples.size(); ++index) {
            const float x0 =
                origin.x + padding + chartWidth * static_cast<float>(index - 1U) / static_cast<float>(props.samples.size() - 1U);
            const float x1 = origin.x + padding + chartWidth * static_cast<float>(index) / static_cast<float>(props.samples.size() - 1U);
            const float y0 = chartBottom - std::clamp(props.samples[index - 1U], 0.0F, 1.0F) * chartHeight;
            const float y1 = chartBottom - std::clamp(props.samples[index], 0.0F, 1.0F) * chartHeight;
            drawList->AddLine({x0, y0}, {x1, y1}, Theme::U32(Theme::Accent()), 1.5F * scale);
        }
    }

    float DrawGlobalDockMetricGrid(const ImVec2 origin, const float width, const std::span<const GlobalDockMetricCardProps> cards,
                                   const Theme::Fonts &fonts) {
        if (cards.empty())
            return 0.0F;
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const bool narrow = width < 900.0F * scale;
        const int columns = narrow ? 2 : 4;
        const auto rows = static_cast<int>((cards.size() + static_cast<std::size_t>(columns) - 1U) / static_cast<std::size_t>(columns));
        const float padding = 10.0F * scale;
        const float gap = 8.0F * scale;
        const float cardHeight = 64.0F * scale;
        const float height = padding * 2.0F + cardHeight * static_cast<float>(rows) + gap * static_cast<float>(rows - 1);
        const float cardWidth =
            std::max(120.0F * scale, (width - padding * 2.0F - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns));
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockContentSurface()));
        drawList->AddLine({origin.x, origin.y + height - scale}, {origin.x + width, origin.y + height - scale},
                          Theme::U32(Theme::Border()));
        for (std::size_t index = 0; index < cards.size(); ++index) {
            const int column = static_cast<int>(index) % columns;
            const int row = static_cast<int>(index) / columns;
            DrawGlobalDockMetricCard({origin.x + padding + static_cast<float>(column) * (cardWidth + gap),
                                      origin.y + padding + static_cast<float>(row) * (cardHeight + gap)},
                                     {cardWidth, cardHeight}, cards[index], fonts);
        }
        return height;
    }

    void DrawGlobalDockMeter(const ImVec2 origin, const float width, const float progress) {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float height = 6.0F * scale;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockControlSurface()),
                                height * 0.5F);
        const float fill = std::clamp(progress, 0.0F, 1.0F) * width;
        if (fill <= 0.0F)
            return;
        drawList->AddRectFilledMultiColor(origin, {origin.x + fill, origin.y + height}, Theme::U32(Theme::Accent()),
                                          Theme::U32(GlobalDockToneColor(GlobalDockTone::Positive)),
                                          Theme::U32(GlobalDockToneColor(GlobalDockTone::Positive)), Theme::U32(Theme::Accent()));
    }

    void DrawGlobalDockFooterSurface(const ImVec2 origin, const float width, const float height) {
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockToolbarSurface()));
        drawList->AddLine(origin, {origin.x + width, origin.y}, Theme::U32(Theme::Border()));
    }

    void DrawGlobalDockStatusFooter(const ImVec2 origin, const float width, const std::span<const std::string_view> segments,
                                    const std::string_view status, const Theme::Fonts &fonts) {
        const float scale = std::max(Theme::GetActiveTokens().sizes.uiScale, 0.01F);
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float footerY = origin.y + (metrics.footerHeight - Theme::TextPx::Caption()) * 0.5F;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        DrawGlobalDockFooterSurface(origin, width, metrics.footerHeight);
        float footerX = origin.x + metrics.contentPadding;
        for (const std::string_view segment : segments) {
            drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {footerX, footerY}, Theme::U32(Theme::Muted()), segment.data(),
                              segment.data() + segment.size());
            footerX += MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Caption(), segment) + 10.0F * scale;
        }
        const float statusX =
            origin.x + width - metrics.contentPadding - MeasureGlobalDockTextWidth(fonts.sansCompact, Theme::TextPx::Caption(), status);
        drawList->AddText(fonts.sansCompact, Theme::TextPx::Caption(), {statusX, footerY}, Theme::U32(Theme::Muted()), status.data(),
                          status.data() + status.size());
    }

    float MeasureGlobalDockToolbarChip(const GlobalDockToolbarChipProps &props, const Theme::Fonts &fonts) {
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float fontSize = Theme::TextPx::Label();
        float width = metrics.toolbarGap * 2.0F + TextWidth(fonts.sansCompact, fontSize, props.label);
        if (props.icon != Ui::UiIcon::None)
            width += 14.0F * Theme::GetActiveTokens().sizes.uiScale + metrics.toolbarGap;
        if (props.count.has_value())
            width += metrics.toolbarGap + TextWidth(fonts.sansCompact, Theme::TextPx::Caption(), std::to_string(*props.count));
        return std::max(metrics.controlHeight, width);
    }

    bool DrawGlobalDockToolbarChip(const ImVec2 origin, const float width, const GlobalDockToolbarChipProps &props,
                                   const Theme::Fonts &fonts) {
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        ImGui::SetCursorScreenPos(origin);
        ImGui::PushID(props.id);
        const bool clicked = ImGui::InvisibleButton("##chip", {width, metrics.controlHeight});
        const bool pressed = clicked && !props.disabled;
        const bool hovered = ImGui::IsItemHovered() && !props.disabled;
        ImGui::PopID();

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        const ImVec2 maximum{origin.x + width, origin.y + metrics.controlHeight};
        drawList->AddRectFilled(origin, maximum, Theme::U32(hovered ? Theme::Hover() : Theme::BottomDockControlSurface()),
                                Theme::GetActiveTokens().radii.control);
        DrawToolbarChipOutline(*drawList, origin, maximum, props, scale);

        const float x = DrawToolbarChipIcon(*drawList, origin, props, fonts, metrics, scale);
        ImFont *font = ResolveFont(fonts.sansCompact);
        const float labelSize = Theme::TextPx::Label();
        const ImVec4 labelColor = ToolbarChipLabelColor(props);
        drawList->AddText(font, labelSize, {x, origin.y + (metrics.controlHeight - labelSize) * 0.5F}, Theme::U32(labelColor),
                          props.label.data(), props.label.data() + props.label.size());
        DrawToolbarChipCount(*drawList, font, origin, maximum, props, metrics);
        return pressed;
    }

    void DrawGlobalDockToolbarSeparator(const float x, const float y) {
        const GlobalDockPaneMetrics metrics = ResolveGlobalDockPaneMetrics();
        const float inset = 5.0F * Theme::GetActiveTokens().sizes.uiScale;
        ImGui::GetWindowDrawList()->AddLine({x, y + inset}, {x, y + metrics.controlHeight - inset}, Theme::U32(Theme::Border()));
    }

    float DrawGlobalDockStatePill(const ImVec2 origin, const std::string_view label, const GlobalDockTone tone, const Theme::Fonts &fonts) {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float fontSize = Theme::TextPx::Caption();
        const float height = 22.0F * scale;
        const float dotSize = 6.0F * scale;
        const float padding = 6.0F * scale;
        const float gap = 5.0F * scale;
        const float width = padding * 2.0F + dotSize + gap + TextWidth(fonts.sansCompact, fontSize, label);
        ImVec4 surface = GlobalDockToneColor(tone);
        surface.w = tone == GlobalDockTone::Neutral ? 0.08F : 0.10F;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(surface), height * 0.5F);
        const ImVec4 color = GlobalDockToneColor(tone);
        drawList->AddCircleFilled({origin.x + padding + dotSize * 0.5F, origin.y + height * 0.5F}, dotSize * 0.5F, Theme::U32(color));
        drawList->AddText(ResolveFont(fonts.sansCompact), fontSize,
                          {origin.x + padding + dotSize + gap, origin.y + (height - fontSize) * 0.5F}, Theme::U32(color), label.data(),
                          label.data() + label.size());
        return width;
    }

    void DrawGlobalDockProgressBar(const ImVec2 origin, const float width, const float progress) {
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float height = 5.0F * scale;
        ImDrawList *drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::BottomDockControlSurface()),
                                height * 0.5F);
        const float fill = std::clamp(progress, 0.0F, 1.0F) * width;
        if (fill > 0.0F)
            drawList->AddRectFilled(origin, {origin.x + fill, origin.y + height}, Theme::U32(Theme::Accent()), height * 0.5F);
    }

    void DrawGlobalDockClippedText(ImDrawList &drawList, ImFont *font, const float fontSize, const ImVec2 minimum, const ImVec2 maximum,
                                   const ImVec4 color, const std::string_view text) {
        if (text.empty() || maximum.x <= minimum.x)
            return;
        drawList.PushClipRect(minimum, maximum, true);
        drawList.AddText(ResolveFont(font), fontSize, minimum, Theme::U32(color), text.data(), text.data() + text.size());
        drawList.PopClipRect();
    }

    void DrawGlobalDockEllipsizedText(ImDrawList &drawList, ImFont *font, const float fontSize, const ImVec2 minimum, const ImVec2 maximum,
                                      const ImVec4 color, const std::string_view text) {
        if (text.empty() || maximum.x <= minimum.x)
            return;
        if (MeasureGlobalDockTextWidth(font, fontSize, text) <= maximum.x - minimum.x) {
            DrawGlobalDockClippedText(drawList, font, fontSize, minimum, maximum, color, text);
            return;
        }
        constexpr std::string_view ellipsis = "…";
        const float ellipsisWidth = MeasureGlobalDockTextWidth(font, fontSize, ellipsis);
        if (ellipsisWidth >= maximum.x - minimum.x)
            return;
        const ImVec2 textMaximum{maximum.x - ellipsisWidth, maximum.y};
        DrawGlobalDockClippedText(drawList, font, fontSize, minimum, textMaximum, color, text);
        drawList.PushClipRect(minimum, maximum, true);
        drawList.AddText(ResolveFont(font), fontSize, {textMaximum.x, minimum.y}, Theme::U32(color), ellipsis.data(),
                         ellipsis.data() + ellipsis.size());
        drawList.PopClipRect();
    }
}  // namespace Horo::Editor
