#include "ScriptModuleContextInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Extensions {
    namespace {
        Result<void> Invalid(std::string detail) {
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid, std::move(detail)));
        }

        bool MemoryPolicy(const ScriptModuleRuntimeLimits &v) {
            return v.maximumSourceBytes > 0 && v.maximumSourceBytes <= 1024U * 1024U && v.maximumMemoryBytes >= 64U * 1024U &&
                   v.maximumMemoryBytes <= 64U * 1024U * 1024U && v.maximumStackBytes >= 16U * 1024U &&
                   v.maximumStackBytes <= 4U * 1024U * 1024U;
        }

        bool RuntimeIdentity(const ScriptModuleRuntimeIdentity &v) {
            return (v.kind == ScriptModuleRuntimeKind::Lua54 || v.kind == ScriptModuleRuntimeKind::JavaScriptNg) && !v.version.empty() &&
                   v.version.size() <= 128 && (v.targetProfile == "headless" || v.targetProfile == "editor-play");
        }

        bool BridgePolicy(const ScriptModuleRuntimeLimits &v) {
            return v.maximumBridgeBytes >= 1024 && v.maximumBridgeBytes <= 64U * 1024U * 1024U && v.maximumBridgeValues > 0 &&
                   v.maximumBridgeValues <= 4096;
        }

        bool LuaPolicy(const ScriptModuleRuntimeLimits &v) {
            return v.luaInstructionBudget >= 1000 && v.luaInstructionBudget <= 10'000'000 && v.luaInstructionBudget % 1000 == 0;
        }

        bool WorkPolicy(const ScriptModuleRuntimeLimits &v) {
            return v.maximumPendingCalls > 0 && v.maximumPendingCalls <= 1024 && v.maximumEventsPerPoll > 0 &&
                   v.maximumEventsPerPoll <= 4096 && v.maximumVmJobsPerPoll > 0 && v.maximumVmJobsPerPoll <= 4096;
        }

        bool SameImport(const ScriptCapabilityImport &a, const ScriptCapabilityImport &b) {
            return a.apiId == b.apiId && a.descriptors->Fingerprint() == b.descriptors->Fingerprint() &&
                   a.provider.Generation() == b.provider.Generation() && a.functions == b.functions &&
                   a.capability.Activation().ModuleId() == b.capability.Activation().ModuleId();
        }

        bool ModuleScopes(const ScriptCapabilityContextDescriptor &policy) {
            constexpr auto count = static_cast<std::size_t>(ScriptCapabilityScopeKind::Count);
            if (policy.scopes.empty() || policy.scopes.size() > count)
                return false;
            std::array<bool, count> seen{};
            for (const auto &scope : policy.scopes) {
                const auto kind = static_cast<std::size_t>(scope.Kind());
                if (kind >= count || seen[kind] || !scope.IsUsable())
                    return false;
                seen[kind] = true;
            }
            return true;
        }

        Result<void> AddImport(ScriptCapabilityContextDescriptor &policy, const ScriptModuleImport &imported) {
            if (!imported.Backend())
                return Result<void>::Success();
            auto value = ScriptModuleContextAccess::Import(*imported.Backend());
            const auto found = std::ranges::find(policy.imports, value.apiId, &ScriptCapabilityImport::apiId);
            if (found == policy.imports.end()) {
                policy.imports.push_back(std::move(value));
                return Result<void>::Success();
            }
            if (!SameImport(*found, value))
                return Invalid("One API alias set contains conflicting provider/schema/function generations.");
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateScriptModuleLimits(const ScriptModuleRuntimeIdentity &runtime, const ScriptModuleRuntimeLimits &v) {
        if (!RuntimeIdentity(runtime))
            return Invalid("Module runtime identity is unsupported or unbounded.");
        if (!MemoryPolicy(v) || !WorkPolicy(v) || !BridgePolicy(v) || !LuaPolicy(v))
            return Invalid("Module memory/work limits are invalid.");
        if (v.executionDeadline <= std::chrono::milliseconds::zero() || v.executionDeadline > std::chrono::minutes{5})
            return Invalid("Module execution requires a bounded deadline.");
        return ValidateScriptValue(ScriptValue::Null(), v.values);
    }

    Result<void> PrepareScriptModuleImports(ScriptCapabilityContextDescriptor &policy, const std::vector<ScriptModuleImport> &imports) {
        if (!policy.imports.empty() || imports.size() > 128)
            return Invalid("Module imports must be derived once from actual host bindings.");
        for (std::size_t index = 0; index < imports.size(); ++index) {
            const auto &entry = imports[index];
            if (std::ranges::any_of(std::span{imports}.first(index), [&](const auto &other) {
                return other.Alias() == entry.Alias();
            }))
                return Invalid("Module local import alias is duplicated.");
            if (entry.Backend() && entry.Resolution().Status() != ExtensionServiceImportStatus::Bound)
                return Invalid("Live module binding does not have a bound resolution.");
            auto added = AddImport(policy, entry);
            if (added.HasError())
                return added;
        }
        return Result<void>::Success();
    }

    namespace {
        Result<void> Creation(const ScriptModuleRuntimeIdentity &runtime, const ScriptModuleRuntimeLimits &limits,
                              ScriptCapabilityContextDescriptor &policy, const std::vector<ScriptModuleImport> &imports) {
            if (auto valid = ValidateScriptModuleLimits(runtime, limits); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            if (!ModuleScopes(policy))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ScriptInvocationInvalid, "Module operation requires unique actual live lifecycle scopes."));
            if (runtime.targetProfile != (policy.profile == ScriptCapabilityProfile::Gameplay ? "editor-play" : "headless"))
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid,
                                                       "Runtime operation profile differs from the actual capability context."));
            if (auto valid = PrepareScriptModuleImports(policy, imports); valid.HasError())
                return Result<void>::Failure(valid.ErrorValue());
            return Result<void>::Success();
        }
    }  // namespace

    ScriptModuleContext::ScriptModuleContext(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

    ScriptModuleContext::~ScriptModuleContext() {
        Close();
    }

    Result<std::shared_ptr<ScriptModuleContext>> ScriptModuleContext::Create(
        std::shared_ptr<ScriptInvocationRegistry> registry, const JobSystem &jobs, ScriptModuleBindingCache &cache,
        ScriptModuleRuntimeIdentity runtime, ScriptCapabilityContextDescriptor policy, std::vector<ScriptModuleImport> imports,
        ScriptModuleRuntimeLimits limits, CancellationToken cancellation, std::optional<ScriptModuleExecutionOwner> executionOwner) {
        using Output = Result<std::shared_ptr<ScriptModuleContext>>;
        if (auto valid = Creation(runtime, limits, policy, imports); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (executionOwner && !executionOwner->capability.IsUsable())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationProviderRevoked));
        auto created =
            ScriptCapabilityContext::Create(registry, policy,
                                            {.parentCancellation = cancellation, .maximumInvocations = limits.maximumPendingCalls});
        if (created.HasError())
            return Output::Failure(created.ErrorValue());
        auto state = std::make_unique<Impl>(std::move(created).Value(), jobs, std::move(runtime), limits, std::move(imports));
        state->registry = std::move(registry);
        state->cache = &cache;
        state->scopes = policy.scopes;
        state->policy = policy;
        state->cancellation = cancellation;
        state->executionOwner = std::move(executionOwner);
        state->work.reserve(limits.maximumPendingCalls);
        state->bound.reserve(state->imports.size());
        for (const auto &entry : state->imports) {
            if (!entry.Backend())
                continue;
            auto binding = state->context.Bind(entry.Backend()->Descriptor().id);
            if (binding.HasError())
                return Output::Failure(binding.ErrorValue());
            auto plan = cache.Acquire(state->runtime, *entry.Backend(), policy);
            if (plan.HasError())
                return Output::Failure(plan.ErrorValue());
            state->bound.push_back({entry.Backend(), std::move(binding).Value(), std::move(plan).Value()});
        }
        return Output::Success(std::shared_ptr<ScriptModuleContext>{new ScriptModuleContext{std::move(state)}});
    }

    std::span<const ScriptModuleImport> ScriptModuleContext::Imports() const noexcept {
        return impl_->imports;
    }

    const ScriptModuleRuntimeIdentity &ScriptModuleContext::Runtime() const noexcept {
        return impl_->runtime;
    }

    const ScriptModuleRuntimeLimits &ScriptModuleContext::Limits() const noexcept {
        return impl_->limits;
    }

    bool ScriptModuleContext::IsUsable() const noexcept {
        return !impl_->sealed.load(std::memory_order_acquire) && ScriptModuleContextOwnersUsable(*impl_);
    }

    void ScriptModuleContext::Close() noexcept {
        if (impl_->closed.exchange(true))
            return;
        impl_->context.Reset();
        // Off-lane scope close does not traverse owner-mutated work metadata. Workers observe canonical context cancellation.
        if (impl_->owner != std::this_thread::get_id())
            return;
        for (const auto &work : impl_->work)
            static_cast<void>(work.job.RequestCancel());
    }

    Result<std::shared_ptr<const ScriptModuleBindingPlan>> ScriptModuleContext::BindingPlan(std::string_view alias) const {
        using Output = Result<std::shared_ptr<const ScriptModuleBindingPlan>>;
        if (impl_->owner != std::this_thread::get_id())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!IsUsable())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        const auto imported = std::ranges::find(impl_->imports, alias, &ScriptModuleImport::Alias);
        if (imported == impl_->imports.end() || !imported->Backend())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        const auto found = std::ranges::find(impl_->bound, imported->Backend(), &Impl::Bound::backend);
        if (found == impl_->bound.end() || found->plan->descriptorFingerprint != found->backend->Evidence().descriptors->Fingerprint())
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        return Output::Success(found->plan);
    }

    Result<ScriptHandle> ScriptModuleContext::IssueHandle(std::string_view alias, std::string type) {
        if (impl_->owner != std::this_thread::get_id())
            return Result<ScriptHandle>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (impl_->sealed.load(std::memory_order_acquire) || !IsUsable())
            return Result<ScriptHandle>::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        const auto entry = std::ranges::find(impl_->imports, alias, &ScriptModuleImport::Alias);
        if (entry == impl_->imports.end() || !entry->Backend())
            return Result<ScriptHandle>::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable));
        const auto bound = std::ranges::find(impl_->bound, entry->Backend(), &Impl::Bound::backend);
        return bound->backend->IssueHandle(bound->capability, std::move(type));
    }

    Result<void> ScriptModuleContext::ValidateHandle(const ScriptHandle &handle) const {
        if (impl_->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!ScriptModuleContextOwnersUsable(*impl_))
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptHandleRevoked));
        for (const auto &bound : impl_->bound)
            if (bound.capability.ValidateHandle(handle).HasValue())
                return Result<void>::Success();
        return Result<void>::Failure(MakeError(ExtensionErrors::ScriptHandleRevoked));
    }
}  // namespace Horo::Extensions
