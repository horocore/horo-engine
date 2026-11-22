#pragma once

#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"

#include <span>

namespace Horo::Editor {
    class WorkspacePanelRegistry;
    class EditorModalHost;
    class IWorkspaceMcpControls;

    /** @brief Registers the closed-by-default persistent navigation view with explicit host capabilities.
     * @param registry Workspace panel registry. @param access Project application capabilities. @param modals GUI modal owner. */
    void RegisterNavigationWorkspacePanel(WorkspacePanelRegistry &registry, NavigationBakeWorkspaceAccess access, EditorModalHost &modals);

    /**
     * @brief Registers the built-in workspace panels.
     * @param registry Target workspace-panel registry.
     */
    void RegisterDefaultWorkspacePanels(WorkspacePanelRegistry &registry);

    /**
     * @brief Registers the default workspace panels and module-provided global-dock panes.
     * @param registry Target workspace-panel registry.
     * @param globalDockPaneFactories Composition-time factories for optional global-dock tabs.
     */
    void RegisterDefaultWorkspacePanels(WorkspacePanelRegistry &registry, std::span<const GlobalDockPaneFactory> globalDockPaneFactories);

    /** @brief Registers workspace panels with locally owned MCP controls in the built-in dock.
     * @param registry Target registry. @param globalDockPaneFactories Optional module-provided panes.
     * @param mcpControls Host-owned local controls that outlive dock attachment.
     */
    void RegisterDefaultWorkspacePanels(WorkspacePanelRegistry &registry, std::span<const GlobalDockPaneFactory> globalDockPaneFactories,
                                        IWorkspaceMcpControls *mcpControls);

}  // namespace Horo::Editor
