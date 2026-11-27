#pragma once

#include "EditorDebugCapture.h"
#include "Horo/Mcp/McpToolRegistry.h"

namespace Horo::Editor {
    inline constexpr std::string_view EditorDebugCaptureGrant = "editor.debug.capture";

    /** @brief Registers the owner-bound capture start, observation, and cancellation tools. */
    [[nodiscard]] Result<std::vector<Mcp::McpToolRegistration>> MakeEditorDebugCaptureMcpTools(
        std::weak_ptr<const EditorDebugCaptureEndpoint> endpoint);
}  // namespace Horo::Editor
