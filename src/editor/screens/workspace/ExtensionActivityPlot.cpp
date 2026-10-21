#include "ExtensionActivityView.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "editor/design_system/components/EditorUiCustomResources.h"

#include <algorithm>
#include <cstdio>
#include <imgui_internal.h>

namespace Horo::Editor {
    namespace {
        /** @brief Draws one copied plot within the host clip rectangle using current semantic tokens. */
        void PaintPlot(const Extensions::EditorUiPlotNode &plot, const Extensions::EditorUiPlotState &state,
                       const Extensions::EditorThemeFrame &theme, const ImVec2 origin, const ImVec2 size, const bool admitted,
                       const bool focused) {
            using namespace Extensions;
            const float scale = theme.uiScale;
            const float width = size.x;
            const float height = size.y;
            const auto color = [&](const EditorThemeColorRole role) {
                const auto value = ColorFor(theme, role);
                return ImGui::GetColorU32(ImVec4{value.red, value.green, value.blue, value.alpha});
            };
            auto *draw = ImGui::GetWindowDrawList();
            const ImVec2 end{origin.x + width, origin.y + height};
            draw->PushClipRect(origin, end, true);
            const auto point = [&](const EditorUiPlotSample &sample) {
                return ImVec2{origin.x + sample.x * width, end.y - sample.y * height};
            };
            const auto lineColor =
                color(admitted ? plot.base.custom.value_or(EditorUiCustomPresentation{}).accent : EditorThemeColorRole::TextDisabled);
            const float stroke = std::max(scale, theme.interaction.focusRingWidth * scale);
            for (std::size_t i = 1; i < plot.samples.size(); ++i)
                draw->AddLine(point(plot.samples[i - 1]), point(plot.samples[i]), lineColor, stroke);
            const auto selected = point(plot.samples[state.selected]);
            const float marker = theme.metrics.smallControlHeight * scale * (1.0F + state.emphasis) / 8.0F;
            draw->AddCircle(selected, marker, color(EditorThemeColorRole::TextPrimary), 0, stroke);
            if (focused)
                draw->AddRect(origin, end, color(EditorThemeColorRole::Focus), 0, 0, stroke);
            draw->PopClipRect();
        }

        /** @brief Owns native navigation keys only while this stable plot item has focus. */
        Extensions::EditorUiPlotInput PlotInput(bool activated) {
            using enum Extensions::EditorUiPlotInput;
            if (ImGui::IsItemFocused()) {
                const auto id = ImGui::GetCurrentContext()->LastItemData.ID;
                ImGui::SetKeyOwner(ImGuiKey_LeftArrow, id);
                ImGui::SetKeyOwner(ImGuiKey_RightArrow, id);
            }
            if (activated)
                return Activate;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                return Select;
            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                return Previous;
            if (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                return Next;
            return None;
        }

        /** @brief Keeps current sample data available without a color-only cue or a per-frame string allocation. */
        void PlotReadout(const Extensions::EditorUiPlotNode &plot, const Extensions::EditorUiPlotState &state, const Theme::Fonts &fonts,
                         Extensions::EditorUiAccessibleNode &semantic) {
            const auto &sample = plot.samples[state.selected];
            char value[96]{};
            std::snprintf(value, sizeof(value), "%zu/%zu  (%.3g, %.3g)", state.selected + 1, plot.samples.size(),
                          static_cast<double>(sample.x), static_cast<double>(sample.y));
            Ui::Hint(value, fonts);
            semantic.selectedIndex = static_cast<std::uint32_t>(state.selected);
            semantic.value = Extensions::EditorUiVectorValue{.components = {sample.x, sample.y}, .dimension = 2};
        }

        /** @brief Borrows one resolved semantic icon from the host registry inside the assigned node clip. */
        void PlotIcon(const Extensions::EditorUiRenderNode &render, const Extensions::EditorThemeFrame &theme, const Theme::Fonts &fonts,
                      ImVec2 origin) {
            if (render.icon == Extensions::EditorThemeIconRole::None)
                return;
            const auto color = Extensions::ColorFor(theme, Extensions::EditorThemeColorRole::TextPrimary);
            const float size = theme.icons.mediumSize * theme.uiScale;
            Ui::DrawEditorIcon(ImGui::GetWindowDrawList(), Ui::EditorUiIcon(render.icon), origin, {size, size},
                               ImGui::GetColorU32({color.red, color.green, color.blue, color.alpha}), fonts.icon);
        }
    }  // namespace

    /** @copydoc ExtensionActivityView::DrawPlot */
    void ExtensionActivityView::DrawPlot(Entry &entry, const Extensions::EditorUiPlotNode &plot, const std::size_t index,
                                         const bool enabled) {
        using namespace Extensions;
        const auto theme = themeCapture_.Frame();
        if (!theme || theme->revision == 0 || plot.samples.size() < 2)
            return;
        const auto fonts = Ui::EditorUiFonts(context_.theme.fonts, entry.render->nodes[index].font);
        Ui::FieldLabel(entry.nodeText[index].c_str(), fonts);
        const float scale = theme->uiScale;
        const float width = std::max(std::min(entry.render->nodes[index].width, entry.offeredWidth - entry.render->nodes[index].x), scale);
        const float height =
            theme->metrics.largeControlHeight * plot.base.custom.value_or(EditorUiCustomPresentation{}).controlRows * scale;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // The host button supplies stable keyboard identity and modal-aware hit testing.
        const bool activated = ImGui::Button("##plot", {width, height});
        const bool focused = ImGui::IsItemFocused();
        const bool hovered = ImGui::IsItemHovered();
        const bool admitted = enabled && (ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled) == 0;
        const auto input = PlotInput(activated);
        const auto retained = std::ranges::find_if(entry.plots, [index](const auto &candidate) {
            return candidate.first == index;
        });
        if (retained == entry.plots.end())
            return;
        auto &state = retained->second;
        const bool invoke = UpdateEditorUiPlot(plot, state, input, (ImGui::GetIO().MousePos.x - origin.x) / width,
                                               admitted && (focused || hovered), *theme, ImGui::GetIO().DeltaTime);
        PaintPlot(plot, state, *theme, origin, {width, height}, admitted, focused);
        auto &semantic = entry.render->accessibility[index];
        PlotReadout(plot, state, fonts, semantic);
        semantic.enabled = admitted;
        semantic.focusable = admitted && !plot.base.readOnly;
        PlotIcon(entry.render->nodes[index], *theme, fonts, origin);
        if (invoke)
            static_cast<void>(host_->QueueAction(entry.projection.surface.descriptor.provider, entry.projection.surface.descriptor.id,
                                                 plot.base.id.value, plot.action.value, entry.projection.revision));
    }
}  // namespace Horo::Editor
