#pragma once
/** @file TriggerVolume.h @brief Static TriggerVolume Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddTriggerVolume(const AddComponentContext &context) noexcept {
        return !context.object.components.triggerVolume;
    }

    /** @brief Inert label/category/action mapping for TriggerVolume. */
    inline constexpr BuiltinAddComponentDefinition TriggerVolume{"component.trigger_volume",
                                                                 "workspace.inspector.kind.trigger_volume",
                                                                 AddComponentCategory::Gameplay,
                                                                 ComponentType::TriggerVolume,
                                                                 CanAddTriggerVolume,
                                                                 false};
}  // namespace Horo::Editor::AddComponentDefinitions
