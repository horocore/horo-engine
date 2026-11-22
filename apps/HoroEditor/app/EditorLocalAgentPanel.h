#pragma once

#include <functional>
#include <memory>

namespace Horo {
    class JobSystem;
    class DurableFileSystem;
}  // namespace Horo

namespace Horo::Security {
    class CredentialVault;
}

namespace Horo::Editor {
    class IWorkspacePanel;

    /** @brief Editor ownership of model work and panel publication, including while the dock is hidden. */
    struct EditorLocalAgentPanelHost final {
        std::shared_ptr<IWorkspacePanel> panel;
        std::function<void()> poll;
    };

    /** @brief Creates the opt-in model panel without performing I/O.
     * @param jobs Existing editor worker owner. @param files Borrowed durable project filesystem.
     * @param cloudCredentials Optional host-owned vault for session-authorized cloud requests.
     * @return Workspace panel and runtime pump that must remain on the editor lane. */
    [[nodiscard]] EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files,
                                                                      std::shared_ptr<Security::CredentialVault> cloudCredentials = {});
}  // namespace Horo::Editor
