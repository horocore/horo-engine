#include "Horo/Extensions/EditorUiForm.h"

#include <algorithm>
#include <cmath>

namespace Horo::Extensions {
    namespace {
        /** @brief Chooses the first closest sample, retaining the current selection when none is within the search bound. */
        void SelectClosestSample(const EditorUiPlotNode &plot, EditorUiPlotState &state, const float normalizedX) noexcept {
            float distance = 2.0F;
            for (std::size_t i = 0; i < plot.samples.size(); ++i) {
                const float candidate = std::abs(plot.samples[i].x - normalizedX);
                if (candidate < distance) {
                    distance = candidate;
                    state.selected = i;
                }
            }
        }
    }  // namespace

    /** @copydoc UpdateEditorUiPlot */
    bool UpdateEditorUiPlot(const EditorUiPlotNode &plot, EditorUiPlotState &state, const EditorUiPlotInput input, const float normalizedX,
                            const bool admitted, const EditorThemeFrame &theme, const float elapsedSeconds) noexcept {
        using enum EditorUiPlotInput;
        if (plot.samples.empty()) {
            state = {};
            return false;
        }
        state.selected = std::min(state.selected, plot.samples.size() - 1);
        const bool enabled = admitted && plot.base.enabled && !plot.base.readOnly;
        if (enabled) {
            if (input == Previous && state.selected > 0)
                --state.selected;
            else if (input == Next && state.selected + 1 < plot.samples.size())
                ++state.selected;
            else if (input == Select && std::isfinite(normalizedX))
                SelectClosestSample(plot, state, normalizedX);
        }
        const float target = enabled ? 1.0F : 0.0F;
        const float duration = MotionFor(theme, plot.base.custom.value_or(EditorUiCustomPresentation{}).motion);
        if (!std::isfinite(state.emphasis))
            state.emphasis = 0;
        if (duration <= 0)
            state.emphasis = target;
        else if (std::isfinite(elapsedSeconds) && elapsedSeconds >= 0) {
            const float step = std::min(elapsedSeconds / duration, 1.0F);
            state.emphasis += std::clamp(target - state.emphasis, -step, step);
        }
        state.emphasis = std::clamp(state.emphasis, 0.0F, 1.0F);
        return enabled && input == Activate;
    }
}  // namespace Horo::Extensions
