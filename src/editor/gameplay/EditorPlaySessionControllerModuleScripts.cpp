#include "EditorPlaySessionController.h"
#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Editor {
    Result<void> EditorPlaySessionController::ConfigureModuleScripts(std::shared_ptr<Extensions::ScriptModuleHostServices> services) {
        if (state_ != EditorPlaySessionState::Idle)
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationInvalid));
        try {
            moduleScripts_ = services ? std::make_unique<EditorPlayModulePrograms>(std::move(services)) : nullptr;
            return Result<void>::Success();
        } catch (...) {
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationAbandoned));
        }
    }

    void EditorPlaySessionController::RefreshModuleScriptEpoch() {
        if (!moduleScripts_)
            return;
        const auto scene = SceneView();
        const auto actual = behaviorOwner_ ? behaviorOwner_->Active() : nullptr;
        if (scene && actual)
            moduleScripts_->AdmitEpoch(scene->RuntimeId(), actual);
        else
            moduleScripts_->CancelEpoch();
    }

    void EditorPlaySessionController::CancelModuleScripts() noexcept {
        if (moduleScripts_)
            moduleScripts_->CancelEpoch();
    }

    Result<void> EditorPlaySessionController::BeginModuleScript(std::shared_ptr<const Extensions::ScriptModuleProgram> program,
                                                                Extensions::ScriptModuleOperationPolicy policy,
                                                                const CancellationToken &cancel) {
        if (!moduleScripts_ || !behaviors_)
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationUnavailable));
        if (state_ != EditorPlaySessionState::Playing && state_ != EditorPlaySessionState::Paused)
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationContextRevoked));
        if (SaveLoading())
            return Result<void>::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationContextRevoked));
        RefreshModuleScriptEpoch();
        return moduleScripts_->Begin(std::move(program), std::move(policy), cancel);
    }

    void EditorPlaySessionController::PollModuleScripts() {
        if (!moduleScripts_)
            return;
        if (SaveLoading()) {
            CancelModuleScripts();
            return;
        }
        RefreshModuleScriptEpoch();
        auto polled = moduleScripts_->Poll();
        if (polled.HasError())
            moduleScriptError_ = polled.ErrorValue();
    }

    std::optional<Extensions::ScriptCallResult> EditorPlaySessionController::TakeModuleScriptResult() {
        return moduleScripts_ ? moduleScripts_->TakeResult() : std::nullopt;
    }
}  // namespace Horo::Editor
