#pragma once

#include "Horo/Editor/IGlobalDockPane.h"
#include "Horo/Editor/NavigationBakeWorkspace.h"

#include <span>

namespace Horo::Editor {
    class WorkspacePanelRegistry;
    class EditorModalHost;

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

}  // namespace Horo::Editor
