#include "Horo/Editor/DefaultWorkspacePanels.h"

#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "editor/screens/workspace/panels/game/GamePanel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyPanel.h"
#include "editor/screens/workspace/panels/inspector/InspectorPanel.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"

namespace Horo::Editor {
    /** @copydoc RegisterDefaultWorkspacePanels(WorkspacePanelRegistry&) */
    void RegisterDefaultWorkspacePanels(WorkspacePanelRegistry &registry) {
        RegisterDefaultWorkspacePanels(registry, {});
    }

    /** @copydoc RegisterDefaultWorkspacePanels(WorkspacePanelRegistry&,std::span<const GlobalDockPaneFactory>) */
    void RegisterDefaultWorkspacePanels(WorkspacePanelRegistry &registry,
                                        const std::span<const GlobalDockPaneFactory> globalDockPaneFactories) {
        registry.RegisterPanel(std::make_shared<HierarchyPanel>());
        registry.RegisterPanel(std::make_shared<InspectorPanel>());
        auto globalDock = std::make_shared<GlobalDockPanel>();
        for (const GlobalDockPaneFactory &factory : globalDockPaneFactories) {
            if (factory)
                static_cast<void>(globalDock->RegisterPane(factory()));
        }
        registry.RegisterPanel(std::move(globalDock));
        registry.RegisterPanel(std::make_shared<ViewportPanel>());
        registry.RegisterPanel(std::make_shared<GamePanel>());
    }
}  // namespace Horo::Editor
