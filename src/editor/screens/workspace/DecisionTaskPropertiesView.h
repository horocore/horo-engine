#pragma once

#include <span>

namespace Horo::AI {
    struct BehaviorTreeProperty;
}

namespace Horo::Editor {
    struct EditorGuiContext;

    /** @brief Draws the immutable authored provider properties of one selected Decision binding.
     * @param properties The canonical, typed source properties, including opaque values that are preserved in source.
     * @param context Editor theme and localization.
     * @param heading Locale key and widget scope for this binding's section.
     */
    void DrawDecisionTaskProperties(std::span<const AI::BehaviorTreeProperty> properties, const EditorGuiContext &context,
                                    const char *heading = "workspace.decision_graph.task_properties");
}  // namespace Horo::Editor
