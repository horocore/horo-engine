#pragma once

/** @file EditorSourceMcpTools.h @brief Project-bound MCP presentation commands for the embedded source editor. */

#include "EditorSourceCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorSourcePresentationGrant = "horo.editor.source.present";

    /** @brief Builds editor-owned presentation registrations without opening a source.
     * @param workspace Exact live workspace endpoint. Registrations retain only weak references.
     * @return A complete pair of registrations or a typed configuration error.
     */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorSourceMcpTools(
        std::weak_ptr<const EditorSourceCommandEndpoint> workspace);
}  // namespace Horo::Editor
