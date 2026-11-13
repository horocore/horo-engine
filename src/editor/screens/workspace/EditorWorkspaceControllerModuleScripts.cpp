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

    Result<void> EditorWorkspaceController::RequestModuleReferenceUpdate(Extensions::ScriptExportVersion version) {
        return m_playSession.RequestModuleReferenceUpdate(version);
    }

    Result<Extensions::ScriptModuleUpdateReport> EditorWorkspaceController::RequestNativeModuleUpdate(
        const Extensions::ScriptModuleProgram &candidate) {
        return m_playSession.RequestNativeModuleUpdate(candidate);
    }

    std::optional<Extensions::ScriptModuleUpdateReport> EditorWorkspaceController::TakeModuleUpdateReport() {
        return m_playSession.TakeModuleUpdateReport();
    }
}  // namespace Horo::Editor
