#pragma once
/** @file Light.h @brief Static Light Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddLight(const AddComponentContext &context) noexcept {
        return !context.object.components.light;
    }

    /** @brief Inert label/category/action mapping for Light. */
    inline constexpr BuiltinAddComponentDefinition Light{"component.light",
                                                         "workspace.inspector.kind.light",
                                                         AddComponentCategory::Rendering,
                                                         ComponentType::Light,
                                                         CanAddLight,
                                                         false};
}  // namespace Horo::Editor::AddComponentDefinitions
