#include "ModuleScriptHostBridgeInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    ModuleScriptHostAuthority::ModuleScriptHostAuthority(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}

    ModuleScriptHostAuthority::~ModuleScriptHostAuthority() = default;

    Result<std::shared_ptr<ModuleScriptHostAuthority>> ModuleScriptHostAuthority::Create(std::shared_ptr<NativeExtensionServiceHost> host) {
        using Output = Result<std::shared_ptr<ModuleScriptHostAuthority>>;
        if (!host)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        auto state = std::make_unique<Impl>(std::move(host));
        if (state->state->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (state->state->closed.load(std::memory_order_acquire))
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationShutdown));
        return Output::Success(std::shared_ptr<ModuleScriptHostAuthority>{new ModuleScriptHostAuthority{std::move(state)}});
    }

    Result<std::shared_ptr<ModuleScriptHostAuthority>> ModuleScriptHostAuthority::Headless(HeadlessExtensionHost &host, bool configuring) {
        auto native = configuring ? NativeExtensionServiceHostAccess::ConfiguringHeadlessServices(host)
                                  : NativeExtensionServiceHostAccess::HeadlessServices(host);
        if (native.HasError())
            return Result<std::shared_ptr<ModuleScriptHostAuthority>>::Failure(native.ErrorValue());
        return Create(std::move(native).Value());
    }

    Result<std::shared_ptr<ModuleScriptHostAuthority>> ModuleScriptHostAuthority::RetainState(NativeExtensionServiceHost &host) {
        using Output = Result<std::shared_ptr<ModuleScriptHostAuthority>>;
        auto value = std::shared_ptr<ModuleScriptHostAuthority>{
            new ModuleScriptHostAuthority{std::make_unique<Impl>(NativeExtensionServiceHostAccess::State(host))}};
        auto open = value->RequireOpenOwner();
        if (open.HasError())
            return Output::Failure(open.ErrorValue());
        return Output::Success(std::move(value));
    }

    const std::shared_ptr<ApplicationCapabilityRegistry> &ModuleScriptHostAuthority::Capabilities() const noexcept {
        return impl_->state->capabilities;
    }

    const std::shared_ptr<BackendServiceRegistry> &ModuleScriptHostAuthority::Services() const noexcept {
        return impl_->state->services;
    }

    const std::shared_ptr<ScriptInvocationRegistry> &ModuleScriptHostAuthority::Invocations() const noexcept {
        return impl_->state->invocations;
    }

    const ExtensionAdmissionPolicy &ModuleScriptHostAuthority::Policy() const noexcept {
        return impl_->state->policy;
    }

    std::thread::id ModuleScriptHostAuthority::OwnerThread() const noexcept {
        return impl_->state->owner;
    }

    std::shared_ptr<const std::atomic<bool>> ModuleScriptHostAuthority::ClosedFlag() const noexcept {
        return {impl_->state, &impl_->state->closed};
    }

    Result<void> ModuleScriptHostAuthority::RequireOpenOwner() const {
        if (OwnerThread() != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (impl_->state->closed.load(std::memory_order_acquire))
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationShutdown));
        return Result<void>::Success();
    }

    Result<std::uint64_t> ModuleScriptHostAuthority::IssueGeneration() {
        auto admitted = RequireOpenOwner();
        if (admitted.HasError())
            return Result<std::uint64_t>::Failure(admitted.ErrorValue());
        auto &next = impl_->state->nextGeneration;
        if (!next)
            return Result<std::uint64_t>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
        return Result<std::uint64_t>::Success(next++);
    }

    Result<ExtensionCapabilityAdmission> ModuleScriptHostAuthority::AdmitReference(std::string moduleId, std::uint64_t generation) const {
        using Output = Result<ExtensionCapabilityAdmission>;
        auto admitted = RequireOpenOwner();
        if (admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        const auto &actual = Policy();
        if (std::ranges::find(actual.availableCapabilities, ExtensionCapabilityId{HORO_EXTENSION_SERVICE_CAPABILITY}) ==
            actual.availableCapabilities.end())
            return Output::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        auto policy = actual;
        policy.availableCapabilities.emplace_back("horo.script.reference");
        return ExtensionCapabilityAdmission::Evaluate({"horo.engine.script-reference",
                                                       std::move(moduleId),
                                                       generation,
                                                       {{{"horo.script.reference"}, {{HORO_EXTENSION_SERVICE_PERMISSION}}}}},
                                                      policy);
    }
}  // namespace Horo::Extensions
