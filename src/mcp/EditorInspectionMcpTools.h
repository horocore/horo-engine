#pragma once

#include "EditorInspectionCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorSceneInspectionGrant = "editor.scene.inspect";
    inline constexpr std::string_view EditorSceneProposalGrant = "editor.scene.propose";

    /** @brief Registers owner-bound inspection and, when supplied, the separate preview-only proposal tool. */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorInspectionMcpTools(
        std::weak_ptr<const EditorInspectionCommandEndpoint> workspace);

    /** @brief Append reference viewport tools whose required workspace operations are composed. */
    void AppendEditorViewportTools(const std::shared_ptr<const EditorInspectionCommandEndpoint> &endpoint,
                                   std::vector<Mcp::McpToolRegistration> &tools);
}  // namespace Horo::Editor
