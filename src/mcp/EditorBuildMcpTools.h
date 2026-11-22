#pragma once

/** @file EditorBuildMcpTools.h @brief Approved editor build commands and read-only operation status. */

#include "EditorBuildCommandEndpoint.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorBuildExecutionGrant = "horo.editor.build.execute";
    inline constexpr std::string_view EditorBuildObservationGrant = "horo.editor.build.observe";

    /** @brief Creates inert editor build start, status, and cancel registrations.
     * @param workspace Borrowed live editor endpoint from host composition.
     * @return Registered commands with distinct effect and observation permissions.
     */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorBuildMcpTools(
        std::weak_ptr<const EditorBuildCommandEndpoint> workspace);
}  // namespace Horo::Editor
