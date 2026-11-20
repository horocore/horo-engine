#pragma once

#include <memory>

namespace Horo::Editor {
    class IWorkspacePanel;

    /** @brief Creates the opt-in dock panel for explicit local inference. Construction performs no I/O. */
    [[nodiscard]] std::shared_ptr<IWorkspacePanel> MakeEditorLocalAgentPanel();
}  // namespace Horo::Editor
