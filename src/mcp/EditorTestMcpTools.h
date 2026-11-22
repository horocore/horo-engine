#pragma once

/** @file EditorTestMcpTools.h @brief Approved native editor test commands and their read-only status. */

#include "EditorTestCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorTestExecutionGrant = "horo.editor.test.execute";
    inline constexpr std::string_view EditorTestObservationGrant = "horo.editor.test.observe";

    /** @brief Creates inert test start, status, and cancel registrations.
     * @param workspace Borrowed live native test endpoint from editor composition.
     * @return Commands with distinct execution and observation permissions.
     */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorTestMcpTools(
        std::weak_ptr<const EditorTestCommandEndpoint> workspace);
}  // namespace Horo::Editor
