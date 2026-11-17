#pragma once
/** @file Camera.h @brief Static Camera Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddCamera(const AddComponentContext &context) noexcept {
        return !context.object.components.camera;
    }

    /** @brief Inert label/category/action mapping for Camera. */
    inline constexpr BuiltinAddComponentDefinition Camera{"component.camera",
                                                          "workspace.inspector.kind.camera",
                                                          AddComponentCategory::Rendering,
                                                          ComponentType::Camera,
                                                          CanAddCamera,
                                                          false};
}  // namespace Horo::Editor::AddComponentDefinitions
