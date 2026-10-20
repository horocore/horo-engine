#include "ExtensionActivityView.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <cstdio>

namespace Horo::Editor {
    /** @copydoc ExtensionActivityView::DrawPlot */
    void ExtensionActivityView::DrawPlot(Entry &entry, const Extensions::EditorUiPlotNode &plot, const std::size_t index) {
        using namespace Extensions;
        const auto theme = themeCapture_.Frame();
        if (!theme || theme->revision == 0 || plot.samples.size() < 2)
            return;
        Ui::FieldLabel(entry.nodeText[index].c_str(), context_.theme.fonts);
        const float scale = theme->uiScale;
        const float width = std::max(ImGui::GetContentRegionAvail().x, scale);
        const float height = theme->metrics.largeControlHeight * 3.0F * scale;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // The host button supplies stable keyboard identity and modal-aware hit testing.
        const bool activated = ImGui::Button("##plot", {width, height});
        const bool focused = ImGui::IsItemFocused();
        const bool hovered = ImGui::IsItemHovered();
        const bool admitted = inputAdmitted_ && plot.base.enabled && entry.projection.surface.activity.enabled;
        EditorUiPlotInput input = EditorUiPlotInput::None;
        if (activated)
            input = EditorUiPlotInput::Activate;
        else if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            input = EditorUiPlotInput::Select;
        else if (focused && ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
            input = EditorUiPlotInput::Previous;
        else if (focused && ImGui::IsKeyPressed(ImGuiKey_RightArrow))
            input = EditorUiPlotInput::Next;
        auto &state = entry.plots[index];
        const bool invoke = UpdateEditorUiPlot(plot, state, input, (ImGui::GetIO().MousePos.x - origin.x) / width,
                                               admitted && (focused || hovered), *theme, ImGui::GetIO().DeltaTime);
        const auto color = [&](const EditorThemeColorRole role) {
            const auto value = ColorFor(*theme, role);
            return ImGui::GetColorU32(ImVec4{value.red, value.green, value.blue, value.alpha});
        };
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 end{origin.x + width, origin.y + height};
        draw->PushClipRect(origin, end, true);
        const auto point = [&](const EditorUiPlotSample &sample) {
            return ImVec2{origin.x + sample.x * width, end.y - sample.y * height};
        };
        const auto lineColor = color(admitted ? EditorThemeColorRole::Accent : EditorThemeColorRole::TextDisabled);
        const float stroke = std::max(scale, theme->interaction.focusRingWidth * scale);
        for (std::size_t i = 1; i < plot.samples.size(); ++i)
            draw->AddLine(point(plot.samples[i - 1]), point(plot.samples[i]), lineColor, stroke);
        const auto selected = point(plot.samples[state.selected]);
        const float marker = theme->metrics.smallControlHeight * scale * (1.0F + state.emphasis) / 8.0F;
        draw->AddCircle(selected, marker, color(EditorThemeColorRole::TextPrimary), 0, stroke);
        if (focused)
            draw->AddRect(origin, end, color(EditorThemeColorRole::Focus), 0, 0, stroke);
        draw->PopClipRect();
        // A text value supplies a non-color cue and exposes exact sample data to keyboard users.
        char value[96]{};
        const auto &sample = plot.samples[state.selected];
        std::snprintf(value, sizeof(value), "%zu/%zu  (%.3g, %.3g)", state.selected + 1, plot.samples.size(), static_cast<double>(sample.x),
                      static_cast<double>(sample.y));
        Ui::Hint(value, context_.theme.fonts);
        if (invoke)
            static_cast<void>(host_->QueueAction(entry.projection.surface.descriptor.provider, entry.projection.surface.descriptor.id,
                                                 plot.base.id.value, plot.action.value, entry.projection.revision));
    }
}  // namespace Horo::Editor
