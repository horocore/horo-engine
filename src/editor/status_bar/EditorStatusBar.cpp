#include "EditorStatusBar.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <format>
#include <ranges>

namespace Horo::Editor {

    namespace {
        constexpr float kOuterPadding = 10.0F;
        constexpr float kItemGap = 8.0F;
        constexpr float kInnerPadding = 7.0F;
        constexpr float kItemHeight = 28.0F;
        constexpr float kIconDiameter = 6.0F;
        constexpr float kOverflowWidth = 28.0F;
        constexpr std::size_t kMaxVisibleItems = 12;

        [[nodiscard]] ImVec4 ToneColor(const EditorStatusItemTone tone) {
            switch (tone) {
                case EditorStatusItemTone::Accent:
                    return Theme::Accent();
                case EditorStatusItemTone::Success:
                    return Theme::Ok();
                case EditorStatusItemTone::Warning:
                    return Theme::Warn();
                case EditorStatusItemTone::Error:
                    return Theme::Err();
                case EditorStatusItemTone::Neutral:
                default:
                    return Theme::Text();
            }
        }

        [[nodiscard]] ImVec4 ToneBackground(const EditorStatusItemTone tone) {
            ImVec4 color = ToneColor(tone);
            color.w = tone == EditorStatusItemTone::Neutral ? 0.06F : 0.12F;
            return color;
        }
    }  // namespace

    EditorStatusBar::EditorStatusBar(const EditorGuiContext &context, EditorStatusItemRegistry &registry)
        : context_(context), registry_(registry) {
        visibleItems_.reserve(EditorStatusItemLimits::MaxItems);
        measuredItems_.reserve(EditorStatusItemLimits::MaxItems);
        rankedItems_.reserve(EditorStatusItemLimits::MaxItems);
        layout_.items.reserve(kMaxVisibleItems);
    }

    float EditorStatusBar::MeasureItem(const EditorStatusItem &item) const {
        const std::string &label = item.descriptor.labelKey.empty()
                                       ? item.content.label
                                       : context_.localization.Get(item.descriptor.localizationNamespace, item.descriptor.labelKey);
        float width = kInnerPadding * 2.0F;
        if (!item.content.iconResourceId.empty()) {
            width += kIconDiameter + 6.0F;
        }
        if (!label.empty()) {
            width += ImGui::CalcTextSize(label.c_str()).x;
        }
        if (!label.empty() && !item.content.value.empty()) {
            width += 6.0F;
        }
        if (!item.content.value.empty()) {
            width += ImGui::CalcTextSize(item.content.value.c_str()).x;
        }
        return std::clamp(width, EditorStatusItemLimits::MinWidth, item.descriptor.maxWidth);
    }

    float EditorStatusBar::PlannedWidth(const EditorStatusItem &item) const {
        const auto measured = std::ranges::find_if(measuredItems_, [&item](const EditorStatusMeasuredItem &candidate) {
            return candidate.item == &item;
        });
        return measured == measuredItems_.end() ? item.descriptor.maxWidth : std::min(measured->measuredWidth, item.descriptor.maxWidth);
    }

    bool EditorStatusBar::DrawItem(const EditorStatusItem &item, const ImVec2 &position, const float width,
                                   const bool interactionEnabled) const {
        const ImVec2 itemSize{width, kItemHeight};
        const ImVec2 itemMax{position.x + itemSize.x, position.y + itemSize.y};
        ImGui::SetCursorScreenPos(position);
        ImGui::PushID(item.descriptor.id.c_str());
        const bool clicked = ImGui::InvisibleButton("##StatusItem", itemSize) && interactionEnabled && item.descriptor.interactive &&
                             !item.descriptor.actionId.empty();
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        ImDrawList *drawList = ImGui::GetWindowDrawList();
        if (item.descriptor.presentation == EditorStatusItemPresentation::Pill ||
            (hovered && interactionEnabled && item.descriptor.interactive)) {
            drawList->AddRectFilled(position, itemMax,
                                    item.descriptor.presentation == EditorStatusItemPresentation::Pill
                                        ? Theme::U32(ToneBackground(item.content.tone))
                                        : Theme::U32(Theme::Hover()),
                                    itemSize.y * 0.5F);
            if (item.descriptor.presentation == EditorStatusItemPresentation::Pill) {
                ImVec4 border = ToneColor(item.content.tone);
                border.w = 0.32F;
                drawList->AddRect(position, itemMax, Theme::U32(border), itemSize.y * 0.5F);
            }
        }

        const std::string &label = item.descriptor.labelKey.empty()
                                       ? item.content.label
                                       : context_.localization.Get(item.descriptor.localizationNamespace, item.descriptor.labelKey);
        float cursorX = position.x + kInnerPadding;
        const float centerY = position.y + itemSize.y * 0.5F;
        if (!item.content.iconResourceId.empty()) {
            drawList->AddCircleFilled(ImVec2(cursorX + kIconDiameter * 0.5F, centerY), kIconDiameter * 0.5F,
                                      Theme::U32(ToneColor(item.content.tone)));
            cursorX += kIconDiameter + 6.0F;
        }

        const ImU32 labelColor =
            Theme::U32(item.descriptor.presentation == EditorStatusItemPresentation::Pill ? ToneColor(item.content.tone) : Theme::Dim());
        const ImU32 valueColor =
            Theme::U32(item.content.tone == EditorStatusItemTone::Neutral ? Theme::Text() : ToneColor(item.content.tone));
        const float textY = position.y + (itemSize.y - ImGui::GetTextLineHeight()) * 0.5F;
        drawList->PushClipRect(ImVec2(position.x + 1.0F, position.y), ImVec2(itemMax.x - 4.0F, itemMax.y), true);
        if (!label.empty()) {
            drawList->AddText(ImVec2(cursorX, textY), labelColor, label.c_str());
            cursorX += ImGui::CalcTextSize(label.c_str()).x;
        }
        if (!label.empty() && !item.content.value.empty()) {
            cursorX += 6.0F;
        }
        if (!item.content.value.empty()) {
            drawList->AddText(ImVec2(cursorX, textY), valueColor, item.content.value.c_str());
        }
        drawList->PopClipRect();

        if (hovered && (MeasureItem(item) >= item.descriptor.maxWidth || item.descriptor.interactive)) {
            Ui::ScopedTooltip tooltip(&context_.theme.fonts);
            if (!label.empty()) {
                ImGui::TextUnformatted(label.c_str());
            }
            if (!item.content.value.empty()) {
                ImGui::SameLine(0.0F, 6.0F);
                ImGui::TextUnformatted(item.content.value.c_str());
            }
        }
        return clicked;
    }

    std::optional<EditorStatusItemInvokedEvent> EditorStatusBar::Draw(const ImVec2 &position, const ImVec2 &size,
                                                                      const EditorStatusBarContext &context,
                                                                      const bool interactionEnabled) {
        ImGui::SetNextWindowPos(position, ImGuiCond_Always);
        ImGui::SetNextWindowSize(size, ImGuiCond_Always);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg0());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0F, 0.0F));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                       (interactionEnabled ? ImGuiWindowFlags_None : ImGuiWindowFlags_NoInputs);
        ImGui::Begin("##EditorStatusBar", nullptr, flags);
        ImGui::GetWindowDrawList()->AddLine(position, ImVec2(position.x + size.x, position.y), Theme::U32(Theme::Border()));

        std::optional<EditorStatusItemInvokedEvent> invocation;
        {
            Theme::ScopedTextStyle textStyle(context_.theme.fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            registry_.CollectVisibleItems(context, visibleItems_);
            measuredItems_.clear();
            for (const EditorStatusItem *item : visibleItems_) {
                measuredItems_.push_back(EditorStatusMeasuredItem{item, MeasureItem(*item)});
            }

            const float availableWidth = std::max(0.0F, size.x - kOuterPadding * 2.0F);
            PlanEditorStatusBarLayoutInto(measuredItems_, availableWidth, kItemGap, kOverflowWidth, kMaxVisibleItems, rankedItems_,
                                          layout_);

            float leftWidth = 0.0F;
            float rightWidth = 0.0F;
            std::size_t leftCount = 0;
            std::size_t rightCount = 0;
            for (const EditorStatusItem *item : layout_.items) {
                float &groupWidth = item->descriptor.alignment == EditorStatusBarAlignment::Left ? leftWidth : rightWidth;
                std::size_t &groupCount = item->descriptor.alignment == EditorStatusBarAlignment::Left ? leftCount : rightCount;
                if (groupCount > 0) {
                    groupWidth += kItemGap;
                }
                groupWidth += PlannedWidth(*item);
                ++groupCount;
            }

            float leftX = position.x + kOuterPadding;
            float rightX = position.x + size.x - kOuterPadding - rightWidth;
            const float itemY = position.y + (size.y - kItemHeight) * 0.5F;
            for (const EditorStatusItem *item : layout_.items) {
                float &x = item->descriptor.alignment == EditorStatusBarAlignment::Left ? leftX : rightX;
                const float itemWidth = PlannedWidth(*item);
                if (DrawItem(*item, ImVec2{x, itemY}, itemWidth, interactionEnabled) && !invocation.has_value()) {
                    invocation = EditorStatusItemInvokedEvent{item->descriptor.id, item->descriptor.actionId};
                }
                x += itemWidth + kItemGap;
            }

            if (layout_.hiddenCount > 0) {
                const std::string overflowLabel = std::format("+{}", layout_.hiddenCount);
                const ImVec2 overflowPos{leftX, itemY};
                ImGui::SetCursorScreenPos(overflowPos);
                ImGui::TextDisabled("%s", overflowLabel.c_str());

                if (ImGui::IsItemHovered()) {
                    Ui::ScopedTooltip tooltip(&context_.theme.fonts);
                    ImGui::Text("%zu", layout_.hiddenCount);
                    ImGui::SameLine(0.0F, 4.0F);
                    ImGui::TextUnformatted(context_.localization.Get("editor", "status.overflow.hidden").c_str());
                }
            }
        }

        ImGui::End();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(2);
        return invocation;
    }
}  // namespace Horo::Editor
