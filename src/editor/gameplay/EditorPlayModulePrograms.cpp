#include "EditorPlayModulePrograms.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <cassert>

namespace Horo::Editor {
    using namespace Extensions;

    EditorPlayModulePrograms::EditorPlayModulePrograms(std::shared_ptr<ScriptModuleHostServices> host) : host_(std::move(host)) {}

    EditorPlayModulePrograms::~EditorPlayModulePrograms() {
        assert(owner_ == std::this_thread::get_id());
        CancelEpoch();
    }

    void EditorPlayModulePrograms::AdmitEpoch(Runtime::SceneRuntimeId scene, const void *actual) {
        assert(owner_ == std::this_thread::get_id());
        if (scene == sceneId_ && actual == actualRuntime_)
            return;
        CancelEpoch();
        if (executing_ || !scene.IsValid() || !actual)
            return;
        runtime_.emplace(ScriptCapabilityScopeKind::Runtime);
        scene_.emplace(ScriptCapabilityScopeKind::Scene);
        sceneId_ = scene;
        actualRuntime_ = actual;
    }

    void EditorPlayModulePrograms::CancelEpoch() noexcept {
        assert(owner_ == std::this_thread::get_id());
        if (runtime_)
            runtime_->Revoke();
        if (scene_)
            scene_->Revoke();
        if (operationScope_)
            operationScope_->Revoke();
        if (operation_)
            operation_->Cancel();
        sceneId_ = {};
        actualRuntime_ = nullptr;
        closePending_ = true;
        if (!executing_)
            FinishCancelled();
    }

    void EditorPlayModulePrograms::FinishCancelled() noexcept {
        operation_.reset();
        operationScope_.reset();
        scene_.reset();
        runtime_.reset();
        closePending_ = false;
    }

    Result<void> EditorPlayModulePrograms::Begin(std::shared_ptr<const ScriptModuleProgram> program, ScriptModuleOperationPolicy policy,
                                                 const CancellationToken &cancel) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (executing_ || operation_)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
        if (!host_ || !runtime_ || !scene_)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        operationScope_.emplace(ScriptCapabilityScopeKind::Operation);
        policy.scopes = {project_.Lease(), runtime_->Lease(), scene_->Lease(), operationScope_->Lease()};
        executing_ = true;
        try {
            auto admitted = host_->Begin(std::move(program), std::move(policy), cancel);
            executing_ = false;
            if (closePending_) {
                FinishCancelled();
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
            }
            if (admitted.HasError()) {
                operationScope_.reset();
                return Result<void>::Failure(admitted.ErrorValue());
            }
            operation_ = std::move(admitted).Value();
            return Result<void>::Success();
        } catch (...) {
            executing_ = false;
            FinishCancelled();
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
        }
    }

    Result<void> EditorPlayModulePrograms::Poll() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (executing_)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        if (!operation_)
            return Result<void>::Success();
        executing_ = true;
        try {
            auto result = operation_->Poll();
            executing_ = false;
            if (closePending_)
                FinishCancelled();
            return result;
        } catch (...) {
            executing_ = false;
            if (closePending_)
                FinishCancelled();
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationAbandoned));
        }
    }

    std::optional<ScriptCallResult> EditorPlayModulePrograms::TakeResult() {
        assert(owner_ == std::this_thread::get_id());
        if (!operation_ || executing_)
            return std::nullopt;
        auto result = operation_->ResultSnapshot();
        if (result) {
            operation_.reset();
            operationScope_.reset();
        }
        return result;
    }
}  // namespace Horo::Editor
