#pragma once

/** @file EditorMcpCommandPanel.h @brief Local operator entry point for project MCP commands. */

#include <memory>

namespace Horo::Editor {
    class EditorMcpWorkspaceHost;
    class IWorkspacePanel;

    /** @brief Creates the editor's local MCP command panel. @param host Borrowed workspace command owner.
     * @return Panel with no active client until the operator explicitly opens one.
     */
    [[nodiscard]] std::shared_ptr<IWorkspacePanel> MakeEditorMcpCommandPanel(EditorMcpWorkspaceHost &host);
}  // namespace Horo::Editor
