#include <thread>
#pragma once
#include "Horo/Runtime/Scene/SceneIdentity.h"
#include "ScriptModuleHostServices.h"

namespace Horo::Editor {
    /** Explicit module operations tied to the actual ready Play Scene and runtime epoch. No object behavior authoring. */
    class EditorPlayModulePrograms final {
    public:
        explicit EditorPlayModulePrograms(std::shared_ptr<Extensions::ScriptModuleHostServices>);
        ~EditorPlayModulePrograms();
        void AdmitEpoch(Runtime::SceneRuntimeId, const void *actualRuntime);
        void CancelEpoch() noexcept;
        Result<void> Begin(std::shared_ptr<const Extensions::ScriptModuleProgram>, Extensions::ScriptModuleOperationPolicy,
                           const CancellationToken &);
        Result<void> Poll();
        /** Queue one explicit additive builtin update for the actual owner Poll safe point. */
        Result<void> RequestReferenceUpdate(Extensions::ScriptExportVersion);
        Result<Extensions::ScriptModuleUpdateReport> RequestNativeUpdate(const Extensions::ScriptModuleProgram &);
        std::optional<Extensions::ScriptModuleUpdateReport> TakeUpdateReport();
        std::optional<Extensions::ScriptCallResult> TakeResult();

    private:
        std::shared_ptr<Extensions::ScriptModuleHostServices> host_;
        Extensions::ScriptCapabilityScope project_{Extensions::ScriptCapabilityScopeKind::Project};
        std::optional<Extensions::ScriptCapabilityScope> runtime_, scene_, operationScope_;
        std::unique_ptr<Extensions::ScriptModuleOperation> operation_;
        Runtime::SceneRuntimeId sceneId_{};
        const void *actualRuntime_{};
        bool executing_{}, closePending_{};
        std::optional<Extensions::ScriptExportVersion> requestedUpdate_;
        std::optional<Extensions::ScriptModuleUpdateReport> updateReport_;
        Result<void> ApplyRequestedUpdate();
        const std::thread::id owner_{std::this_thread::get_id()};
        void FinishCancelled() noexcept;
    };
}  // namespace Horo::Editor
