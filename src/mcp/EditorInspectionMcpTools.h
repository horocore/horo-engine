#pragma once

#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorSceneInspectionGrant = "editor.scene.inspect";
    inline constexpr std::string_view EditorSceneProposalGrant = "editor.scene.propose";

    /** @brief Registers owner-bound inspection and, when supplied, the separate preview-only proposal tool. */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace);
}  // namespace Horo::Editor
