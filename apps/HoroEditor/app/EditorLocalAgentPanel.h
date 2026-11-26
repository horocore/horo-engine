#pragma once

#include "EditorProviderOperationBudget.h"

#include <functional>
#include <memory>

namespace Horo {
    class JobSystem;
    class DurableFileSystem;
    class IExternalProcessRunner;
}  // namespace Horo

namespace Horo::Editor {
    class IWorkspacePanel;
    class EditorCloudModelProfileStore;
    class EditorImmersiveListening;

    /** @brief Selects which model-provider residences the editor host can expose. */
    enum class EditorModelProviderPolicy {
        AllowCloud,
        LocalOnly
    };

    /** @brief Provider policy and operation limits fixed at editor startup. */
    struct EditorModelProviderOptions final {
        EditorProviderOperationLimits limits;
        EditorModelProviderPolicy policy{EditorModelProviderPolicy::AllowCloud};
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
     * @param options Provider eligibility and optional model and speech operation ceilings for this editor session.
     * @return Workspace panel and runtime pump that must remain on the editor lane. */
    [[nodiscard]] EditorLocalAgentPanelHost MakeEditorLocalAgentPanel(const JobSystem &jobs, DurableFileSystem &files,
                                                                      std::shared_ptr<EditorCloudModelProfileStore> cloudProfiles = {},
                                                                      IExternalProcessRunner *processes = nullptr,
                                                                      EditorImmersiveListening *listening = nullptr,
                                                                      EditorModelProviderOptions options = {});
}  // namespace Horo::Editor
