#include "EditorPlayModulePrograms.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <cassert>

namespace Horo::Editor {
    using namespace Extensions;

    Result<void> EditorPlayModulePrograms::RequestReferenceUpdate(ScriptExportVersion version) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (executing_ || requestedUpdate_)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
        if (!runtime_ || !scene_ || !operation_)
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        requestedUpdate_ = version;
        return Result<void>::Success();
    }

    Result<void> EditorPlayModulePrograms::ApplyRequestedUpdate() {
        if (!requestedUpdate_)
            return Result<void>::Success();
        const auto version = *requestedUpdate_;
        requestedUpdate_.reset();
        auto applied = host_->RefreshReference(version);
        if (applied.HasError()) {
            updateReport_ = ScriptModuleUpdateReport{.kind = applied.ErrorValue().code.Value() ==
                                                                     ExtensionErrors::ScriptExportDescriptorIncompatible.code.Value()
                                                                 ? ScriptModuleUpdateKind::Breaking
                                                                 : ScriptModuleUpdateKind::Unavailable,
                                                     .diagnostic = applied.ErrorValue()};
            return Result<void>::Failure(applied.ErrorValue());
        }
        if (!applied.Value().empty())
            updateReport_ = std::move(applied).Value().front();
        return Result<void>::Success();
    }

    Result<ScriptModuleUpdateReport> EditorPlayModulePrograms::RequestNativeUpdate(const ScriptModuleProgram &candidate) {
        if (owner_ != std::this_thread::get_id())
            return Result<ScriptModuleUpdateReport>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (executing_ || !host_ || !runtime_ || !scene_)
            return Result<ScriptModuleUpdateReport>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        return host_->RequestNativeUpdate(candidate);
    }

    std::optional<ScriptModuleUpdateReport> EditorPlayModulePrograms::TakeUpdateReport() {
        assert(owner_ == std::this_thread::get_id());
        auto result = std::move(updateReport_);
        updateReport_.reset();
        return result;
    }
}  // namespace Horo::Editor
