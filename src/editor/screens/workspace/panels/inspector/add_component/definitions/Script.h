#pragma once
/** @file Script.h @brief Static Script Add Component contribution; no ambient registration. */
#include "../AddComponentRegistry.h"

namespace Horo::Editor::AddComponentDefinitions {
    /** @brief Reads only current selection and detached draft availability. */
    inline bool CanAddScript(const AddComponentContext &context) noexcept {
        return context.hasProject;
    }

    /** @brief Inert label/category/action mapping for Script. */
    inline constexpr BuiltinAddComponentDefinition Script{"workflow.create_script",
                                                          "workspace.inspector.create_script",
                                                          AddComponentCategory::Scripts,
                                                          AddComponentWorkflow::CreateScript,
                                                          CanAddScript,
                                                          true};
}  // namespace Horo::Editor::AddComponentDefinitions
