#pragma once

#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorSceneInspectionGrant = "editor.scene.inspect";

    /** @brief Registers the read-only owner-bound inspection tool. */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace);
}  // namespace Horo::Editor
