#include "ScriptModuleHostServicesInternal.h"
#include "ScriptModuleProjection.h"

#include <cassert>

namespace Horo::Extensions {
    ScriptModuleOperation::Impl::Impl(std::shared_ptr<ScriptModuleHostServices> host, std::unique_ptr<IScriptModuleRuntime> vm,
                                      std::shared_ptr<ScriptModuleContext> admitted, std::shared_ptr<const ScriptModuleProgram> captured)
        : owner(std::move(host)), runtime(std::move(vm)), context(std::move(admitted)), program(std::move(captured)),
          emergency(MakeScriptModuleEmergencyResult()) {}

    ScriptModuleOperation::ScriptModuleOperation(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}

    ScriptModuleOperation::Impl::~Impl() {
        if (counted)
            --owner->impl_->activeOperations;
    }

    ScriptModuleOperation::~ScriptModuleOperation() {
        assert(impl_->lane == std::this_thread::get_id());
        Cancel();
        for (auto &operation : impl_->owner->impl_->operations)
            if (operation == this)
                operation = nullptr;
    }

    void ScriptModuleOperation::Cancel() noexcept {
        impl_->runtime->Close();
    }

    Result<void> ScriptModuleOperation::Poll() {
        if (impl_->lane != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (impl_->result)
            return Result<void>::Success();
        auto advanced = impl_->runtime->Poll();
        if (advanced.HasError()) {
            SetScriptModuleFailure(impl_->result, impl_->emergency, advanced.ErrorValue(), {});
            Cancel();
            return advanced;
        }
        auto retired = impl_->owner->PollProviderRetirement();
        if (retired.HasError())
            return Result<void>::Failure(retired.ErrorValue());
        impl_->result = impl_->runtime->ResultSnapshot();
        if (impl_->result)
            Cancel();
        return Result<void>::Success();
    }

    std::optional<ScriptCallResult> ScriptModuleOperation::ResultSnapshot() const {
        if (impl_->lane != std::this_thread::get_id())
            return std::nullopt;
        return impl_->result ? impl_->result : impl_->runtime->ResultSnapshot();
    }
}  // namespace Horo::Extensions
