#pragma once

#include <functional>
#include <memory>

namespace Horo {
    class JobSystem;
    class DurableFileSystem;
}  // namespace Horo

namespace Horo::Editor {
    class IWorkspacePanel;
    class EditorCloudModelProfileStore;

    /** @brief Editor ownership of model work and panel publication, including while the dock is hidden. */
    struct EditorLocalAgentPanelHost final {
        std::shared_ptr<IWorkspacePanel> panel;
        std::function<void()> poll;
    };

    /** @brief Creates the opt-in model panel without performing I/O.
     * @param jobs Existing editor worker owner. @param files Borrowed durable project filesystem.
     * @param cloudProfiles Optional host-owned user-profile and credential authority.
     * @return Workspace panel and runtime pump that must remain on the editor lane. */
    [[nodiscard]] EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files,
                                                                      std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles = {});
}  // namespace Horo::Editor
