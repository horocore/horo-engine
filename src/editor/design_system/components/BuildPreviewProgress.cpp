#include "editor/design_system/components/BuildPreviewProgress.h"

#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <imgui.h>

namespace Horo::Editor::Ui {
    void BuildPreviewPipeline(const std::span<const BuildPreviewPipelineStep> steps, const Theme::Fonts &fonts) {
        if (steps.empty())
            return;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ScaledLayoutValue(52.0F);
        ImGui::Dummy({width, height});
        ImDrawList *const drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::Bg0()));
        drawList->AddLine({origin.x, origin.y + height - 1.0F}, {origin.x + width, origin.y + height - 1.0F}, Theme::U32(Theme::Border()));

        const float inset = ScaledLayoutValue(20.0F);
        const float gap = ScaledLayoutValue(5.0F);
        const float segmentWidth =
            std::max(0.0F, (width - inset * 2.0F - gap * static_cast<float>(steps.size() - 1)) / static_cast<float>(steps.size()));
        ImFont *const font = fonts.sansCompact ? fonts.sansCompact : ImGui::GetFont();
        const float labelSize = Theme::TextPx::Caption();
        for (std::size_t index = 0; index < steps.size(); ++index) {
            const BuildPreviewPipelineStep &step = steps[index];
            const float x = origin.x + inset + static_cast<float>(index) * (segmentWidth + gap);
            const float top = origin.y + ScaledLayoutValue(12.0F);
            const ImVec2 barMin{x, top};
            const ImVec2 barMax{x + segmentWidth, top + ScaledLayoutValue(4.0F)};
            drawList->AddRectFilled(barMin, barMax, Theme::U32(Theme::Bg3()), ScaledLayoutValue(2.0F));
            const float fill = step.completed ? 1.0F : step.current ? std::clamp(step.progress, 0.0F, 1.0F) : 0.0F;
            const float indicator = step.current ? ScaledLayoutValue(3.0F) : 0.0F;
            const float fillWidth = std::min(segmentWidth, std::max(segmentWidth * fill, indicator));
            if (fillWidth > 0.0F)
                drawList->AddRectFilled(barMin, {x + fillWidth, barMax.y}, Theme::U32(Theme::Ok()), ScaledLayoutValue(2.0F));
            const std::string caption = std::format("{:02} {}", index + 1, step.label);
            drawList->AddText(font, labelSize, {x, top + ScaledLayoutValue(12.0F)},
                              Theme::U32(step.current ? Theme::Text() : Theme::Muted()), caption.c_str());
        }
    }

    void BuildPreviewSpinner() {
        constexpr float kPi = 3.14159265F;
        const float diameter = ScaledLayoutValue(16.0F);
        const ImVec2 topLeft = ImGui::GetCursorScreenPos();
        ImGui::Dummy({diameter, diameter});
        const ImVec2 center{topLeft.x + diameter * 0.5F, topLeft.y + diameter * 0.5F};
        const float radius = diameter * 0.38F;
        const float start = static_cast<float>(ImGui::GetTime()) * 4.0F;
        constexpr int segments = 18;
        for (int index = 0; index < segments; ++index) {
            const float a = start + static_cast<float>(index) * (1.5F * kPi / static_cast<float>(segments));
            const float b = start + static_cast<float>(index + 1) * (1.5F * kPi / static_cast<float>(segments));
            const ImVec2 from{center.x + std::cos(a) * radius, center.y + std::sin(a) * radius};
            const ImVec2 to{center.x + std::cos(b) * radius, center.y + std::sin(b) * radius};
            ImGui::GetWindowDrawList()->AddLine(from, to, ImGui::GetColorU32(Theme::Accent()), ScaledLayoutValue(2.0F));
        }
    }

    void BuildPreviewFieldLabel(const char *label, const Theme::Fonts &fonts) {
        Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
        ImGui::TextColored(Theme::Text(), "%s", label);
    }

    void BuildPreviewSectionHeading(const char *label, const Theme::Fonts &fonts, const bool divided) {
        if (divided) {
            ImGui::Dummy({0.0F, ScaledLayoutValue(15.0F)});
            ImGui::Separator();
            ImGui::Dummy({0.0F, ScaledLayoutValue(15.0F)});
        }
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        {
            Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            ImGui::TextColored(Theme::Muted(), "%s", label);
        }
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const float lineStart = max.x + ScaledLayoutValue(10.0F);
        if (lineStart < right)
            ImGui::GetWindowDrawList()->AddLine({lineStart, (min.y + max.y) * 0.5F}, {right, (min.y + max.y) * 0.5F},
                                                Theme::U32(Theme::Border()));
        ImGui::Dummy({0.0F, ScaledLayoutValue(8.0F)});
    }

    void BuildPreviewNotice(const char *id, const char *message, const Theme::Fonts &fonts) {
        const float width = ImGui::GetContentRegionAvail().x;
        Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
        const float textHeight = ImGui::CalcTextSize(message, nullptr, false, std::max(1.0F, width - ScaledLayoutValue(32.0F))).y;
        const float height = std::max(ScaledLayoutValue(44.0F), textHeight + ScaledLayoutValue(24.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ScaledLayoutValue(16.0F), ScaledLayoutValue(12.0F)});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        ImGui::BeginChild(id, {0.0F, height}, true,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
        ImGui::TextWrapped("%s", message);
        ImGui::PopStyleColor();
        const ImVec2 position = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine({position.x + 1.0F, position.y + 1.0F},
                                            {position.x + 1.0F, position.y + ImGui::GetWindowHeight() - 1.0F}, Theme::U32(Theme::Accent()),
                                            ScaledLayoutValue(3.0F));
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    void BuildPreviewReview(const char *heading, const char *description, const char *notice,
                            const std::span<const BuildPreviewReviewSection> sections, const Theme::Fonts &fonts) {
        const float availableWidth = ImGui::GetContentRegionAvail().x;
        float headingWidth = 0.0F;
        {
            Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Label(), Theme::FontPx::SansCompact);
            headingWidth = ImGui::CalcTextSize(heading).x;
            ImGui::TextUnformatted(heading);
        }
        {
            Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            const float descriptionWidth = ImGui::CalcTextSize(description).x;
            if (headingWidth + descriptionWidth + ScaledLayoutValue(16.0F) <= availableWidth)
                ImGui::SameLine(0.0F, ScaledLayoutValue(16.0F));
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
            ImGui::TextWrapped("%s", description);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy({0.0F, ScaledLayoutValue(10.0F)});

        float noticeHeight = ScaledLayoutValue(44.0F);
        {
            Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
            const float textWidth = std::max(1.0F, ImGui::GetContentRegionAvail().x - ScaledLayoutValue(32.0F));
            noticeHeight = std::max(noticeHeight, ImGui::CalcTextSize(notice, nullptr, false, textWidth).y + ScaledLayoutValue(24.0F));
        }
        const float gap = ScaledLayoutValue(14.0F);
        const float tableHeight = std::max(ScaledLayoutValue(80.0F), ImGui::GetContentRegionAvail().y - noticeHeight - gap);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Theme::GetActiveTokens().radii.control);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
        if (ImGui::BeginChild("##buildPreviewReviewTable", {0.0F, tableHeight}, true, ImGuiWindowFlags_AlwaysUseWindowPadding)) {
            ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2{ScaledLayoutValue(14.0F), ScaledLayoutValue(8.0F)});
            ImGui::PushStyleColor(ImGuiCol_TableBorderLight, Theme::Border());
            if (ImGui::BeginTable("##reviewSections", 2,
                                  ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX)) {
                ImGui::TableSetupColumn("##reviewLabel", ImGuiTableColumnFlags_WidthFixed, ScaledLayoutValue(190.0F));
                ImGui::TableSetupColumn("##reviewValue", ImGuiTableColumnFlags_WidthStretch);
                for (std::size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
                    const BuildPreviewReviewSection &section = sections[sectionIndex];
                    ImGui::TableNextRow();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, Theme::U32(Theme::Bg1()));
                    ImGui::TableSetColumnIndex(0);
                    {
                        Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                        ImGui::TextColored(sectionIndex == 0 ? Theme::Accent() : Theme::Dim(), "%s", section.title.c_str());
                    }
                    for (const BuildPreviewReviewField &field : section.fields) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        {
                            Theme::ScopedTextStyle style(fonts.sansCompact, Theme::TextPx::Caption(), Theme::FontPx::SansCompact);
                            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Muted());
                            ImGui::TextWrapped("%s", field.label.c_str());
                            ImGui::PopStyleColor();
                        }
                        ImGui::TableSetColumnIndex(1);
                        {
                            Theme::ScopedTextStyle style(fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
                            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
                            ImGui::TextWrapped("%s", field.value.c_str());
                            ImGui::PopStyleColor();
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
        ImGui::Dummy({0.0F, gap});
        BuildPreviewNotice("##buildPreviewReviewNotice", notice, fonts);
    }
}  // namespace Horo::Editor::Ui
