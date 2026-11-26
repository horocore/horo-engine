#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Horo {
    class JobSystem;
    class DurableFileSystem;
    class IExternalProcessRunner;
}  // namespace Horo

namespace Horo::Editor {
    class IWorkspacePanel;
    class EditorCloudModelProfileStore;
    class EditorImmersiveListening;

    /** @brief Limits accepted model operations across the editor session and for each project within it. */
    struct EditorModelOperationLimits final {
        std::optional<std::uint32_t> session; /**< Unset leaves the session uncapped. */
        std::optional<std::uint32_t> project; /**< Unset leaves individual projects uncapped. */
    };

    /** @brief Editor ownership of model work and panel publication, including while the dock is hidden. */
    struct EditorLocalAgentPanelHost final {
        std::shared_ptr<IWorkspacePanel> panel;
        std::function<void()> poll;
    };

    /** @brief Creates the opt-in model panel without performing I/O.
     * @param jobs Existing editor worker owner. @param files Borrowed durable project filesystem.
     * @param cloudProfiles Optional host-owned user-profile and credential authority.
     * @param processes Optional host process authority for an isolated C++ diagnostic preview. Outlives the panel.
     * @param listening Optional host-owned immersive listening composition.
     * @param limits Optional local and cloud model operation ceilings for this editor session.
     * @return Workspace panel and runtime pump that must remain on the editor lane. */
    [[nodiscard]] EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files,
                                                                      std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles = {},
                                                                      IExternalProcessRunner *processes = nullptr,
                                                                      EditorImmersiveListening *listening = nullptr,
                                                                      EditorModelOperationLimits limits = {});
}  // namespace Horo::Editor
