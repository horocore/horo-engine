#include "EditorWorkspaceController.h"

namespace Horo::Editor {
    Result<void> EditorWorkspaceController::BeginModuleProgram(std::shared_ptr<const Extensions::ScriptModuleProgram> program,
                                                               Extensions::ScriptModuleOperationPolicy policy,
                                                               const CancellationToken &cancel) {
        return m_playSession.BeginModuleScript(std::move(program), std::move(policy), cancel);
    }

    std::optional<Extensions::ScriptCallResult> EditorWorkspaceController::TakeModuleProgramResult() {
        return m_playSession.TakeModuleScriptResult();
    }
}  // namespace Horo::Editor
