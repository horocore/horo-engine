#include "ScriptModuleHostServicesInternal.h"
#include "ScriptModuleProjection.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        constexpr auto ReferenceAlias = "horo.reference";

        /** @brief Checks the project/context intersection used by a script operation. */
        bool AllowsCapability(const ScriptModuleOperationPolicy &policy, const std::string_view capability) {
            return std::ranges::find(policy.projectCapabilities, capability, &ExtensionCapabilityId::value) !=
                       policy.projectCapabilities.end() &&
                   std::ranges::find(policy.contextCapabilities, capability, &ExtensionCapabilityId::value) !=
                       policy.contextCapabilities.end();
        }

        bool ReferenceAllowed(const ScriptModuleOperationPolicy &policy) {
            return AllowsCapability(policy, "horo.script.reference");
        }

        Result<void> Admit(const auto &state, const std::shared_ptr<const ScriptModuleProgram> &program,
                           const ScriptModuleOperationPolicy &policy, const CancellationToken &cancellation) {
            if (state.owner != std::this_thread::get_id())
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
            if (!program || !policy.nativeConsent || !policy.scriptConsent || cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
            if (state.activeOperations >= state.MaximumOperations)
                return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationCapacityExceeded));
            if (program->RequestsReference() && !ReferenceAllowed(policy))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::CapabilityUnavailable, "The requested builtin API lacks project/context approval."));
            return ValidateScriptModuleActiveCapture(*state.authority, *program);
        }

        Result<std::vector<ScriptModuleImport>> Imports(auto &state, const ScriptModuleProgram &program) {
            std::vector<ScriptModuleImport> result;
            result.reserve(program.Module().imports.size() + 1);
            for (const auto &declared : program.Module().imports) {
                auto imported = BindModuleScriptHostImport(*state.authority, program.Manifest().id, program.Module().id, declared.id,
                                                           declared.id, state.profile);
                if (imported.HasError())
                    return Result<std::vector<ScriptModuleImport>>::Failure(imported.ErrorValue());
                result.push_back(std::move(imported).Value());
            }
            if (program.RequestsReference()) {
                auto reference = state.reference->Bind(ReferenceAlias);
                if (reference.HasError())
                    return Result<std::vector<ScriptModuleImport>>::Failure(reference.ErrorValue());
                result.push_back(std::move(reference).Value());
            }
            return Result<std::vector<ScriptModuleImport>>::Success(std::move(result));
        }

        Result<std::shared_ptr<ScriptModuleContext>> Context(auto &state, const ScriptModuleProgram &program,
                                                             ScriptModuleOperationPolicy policy, const CancellationToken &cancellation) {
            auto imported = Imports(state, program);
            if (imported.HasError())
                return Result<std::shared_ptr<ScriptModuleContext>>::Failure(imported.ErrorValue());
            auto source = state.authority->Source(program.Manifest().id, program.Module().id);
            if (source.HasError())
                return Result<std::shared_ptr<ScriptModuleContext>>::Failure(source.ErrorValue());
            ScriptModuleExecutionOwner owner{source.Value().Capability(), source.Value().Code()};
            const std::string profile = state.profile == ScriptCapabilityProfile::Gameplay ? "editor-play" : "headless";
            auto identity = program.Runtime() == ScriptModuleRuntimeKind::Lua54 ? LuaScriptModuleRuntimeIdentity(profile)
                                                                                : JavaScriptModuleRuntimeIdentity(profile);
            return ScriptModuleContext::Create(state.authority->Invocations(), state.jobs, state.cache, std::move(identity),
                                               ScriptModuleOperationContext(program, std::move(policy), state.profile,
                                                                            state.authority->Policy().revision),
                                               std::move(imported).Value(), {}, cancellation, std::move(owner));
        }

        Result<std::unique_ptr<IScriptModuleRuntime>> Runtime(ScriptModuleRuntimeKind kind, std::shared_ptr<ScriptModuleContext> context) {
            if (kind == ScriptModuleRuntimeKind::Lua54)
                return CreateLuaScriptModuleRuntime(std::move(context));
            return CreateJavaScriptModuleRuntime(std::move(context));
        }

        /** @brief Copies only declaration data from the current import and removes unadmitted functions. */
        ScriptModuleApiMetadata DescribeImport(std::string alias, const Result<ScriptModuleImport> &imported) {
            if (imported.HasError())
                return {std::move(alias), std::nullopt, imported.ErrorValue()};
            const auto &bound = imported.Value();
            if (!bound.Backend())
                return {std::move(alias), std::nullopt, bound.Unavailable()};
            auto descriptor = bound.Backend()->Descriptor();
            std::erase_if(descriptor.functions, [&](const ScriptExportFunctionDescriptor &function) {
                return !bound.Backend()->AllowsFunction(function.id);
            });
            return {std::move(alias), std::move(descriptor), std::nullopt};
        }

        /** @brief Reads the builtin's installed descriptor without preparing or reloading a provider. */
        ScriptModuleApiMetadata DescribeReference(const ScriptModuleReferenceInstallation *reference,
                                                  const ScriptModuleOperationPolicy &policy) {
            if (!ReferenceAllowed(policy))
                return {ReferenceAlias, std::nullopt, MakeError(ExtensionErrors::CapabilityUnavailable)};
            if (!reference)
                return {ReferenceAlias, std::nullopt, MakeError(ExtensionErrors::ScriptInvocationUnavailable)};
            return DescribeImport(ReferenceAlias, reference->Bind(ReferenceAlias));
        }
    }  // namespace

    ScriptModuleHostServices::ScriptModuleHostServices(std::unique_ptr<Impl> value) : impl_(std::move(value)) {}

    ScriptModuleHostServices::~ScriptModuleHostServices() = default;

    Result<std::shared_ptr<ScriptModuleHostServices>> ScriptModuleHostServices::Create(std::shared_ptr<NativeExtensionServiceHost> native,
                                                                                       const JobSystem &jobs,
                                                                                       ScriptCapabilityProfile profile) {
        auto authority = ModuleScriptHostAuthority::Create(std::move(native));
        if (authority.HasError())
            return Result<std::shared_ptr<ScriptModuleHostServices>>::Failure(authority.ErrorValue());
        return Create(std::move(authority).Value(), jobs, profile);
    }

    Result<std::shared_ptr<ScriptModuleHostServices>> ScriptModuleHostServices::Create(std::shared_ptr<ModuleScriptHostAuthority> authority,
                                                                                       const JobSystem &jobs,
                                                                                       ScriptCapabilityProfile profile) {
        using Output = Result<std::shared_ptr<ScriptModuleHostServices>>;
        if (!authority || profile >= ScriptCapabilityProfile::Count)
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationInvalid));
        if (auto lane = authority->RequireOpenOwner(); lane.HasError())
            return Output::Failure(lane.ErrorValue());
        return Output::Success(std::shared_ptr<ScriptModuleHostServices>{
            new ScriptModuleHostServices{std::make_unique<Impl>(std::move(authority), jobs, profile)}});
    }

    Result<std::shared_ptr<ScriptModuleHostServices>> ScriptModuleHostServices::CreateConfiguringHeadless(HeadlessExtensionHost &host,
                                                                                                          const JobSystem &jobs) {
        auto authority = ModuleScriptHostAuthority::Headless(host, true);
        if (authority.HasError())
            return Result<std::shared_ptr<ScriptModuleHostServices>>::Failure(authority.ErrorValue());
        return Create(std::move(authority).Value(), jobs, ScriptCapabilityProfile::Tooling);
    }

    Result<void> ScriptModuleHostServices::PrepareReference(bool requested) {
        if (impl_->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(ExtensionErrors::ScriptInvocationThreadViolation));
        if (!requested || impl_->reference)
            return Result<void>::Success();
        auto reference = ScriptModuleReferenceInstallation::Create(impl_->authority, impl_->profile);
        if (reference.HasError())
            return Result<void>::Failure(reference.ErrorValue());
        impl_->reference = std::move(reference).Value();
        return Result<void>::Success();
    }

    ScriptModuleBindingCacheStats ScriptModuleHostServices::CacheStats() const noexcept {
        return impl_->cache.Stats();
    }

    /** @copydoc ScriptModuleHostServices::DescribeApis */
    Result<std::vector<ScriptModuleApiMetadata>> ScriptModuleHostServices::DescribeApis(const ScriptModuleProgram &program,
                                                                                        const ScriptModuleOperationPolicy &policy) const {
        using Output = Result<std::vector<ScriptModuleApiMetadata>>;
        if (!policy.nativeConsent || !policy.scriptConsent)
            return Output::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        if (std::ranges::none_of(policy.scopes, [](const ScriptCapabilityScopeLease &scope) {
            return scope.Kind() == ScriptCapabilityScopeKind::Project && scope.IsUsable();
        }))
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationContextRevoked));
        if (auto current = ValidateScriptModuleActiveCapture(*impl_->authority, program); current.HasError())
            return Output::Failure(current.ErrorValue());
        std::vector<ScriptModuleApiMetadata> result;
        result.reserve(program.Module().imports.size() + 1);
        for (const auto &declared : program.Module().imports) {
            if (!AllowsCapability(policy, declared.service)) {
                result.push_back({declared.id, std::nullopt, MakeError(ExtensionErrors::CapabilityUnavailable)});
                continue;
            }
            const auto imported = BindModuleScriptHostImport(*impl_->authority, program.Manifest().id, program.Module().id, declared.id,
                                                             declared.id, impl_->profile);
            result.push_back(DescribeImport(declared.id, imported));
        }
        if (program.RequestsReference())
            result.push_back(DescribeReference(impl_->reference.get(), policy));
        return Output::Success(std::move(result));
    }

    Result<std::unique_ptr<ScriptModuleOperation>> ScriptModuleHostServices::Begin(std::shared_ptr<const ScriptModuleProgram> program,
                                                                                   ScriptModuleOperationPolicy policy,
                                                                                   const CancellationToken &cancellation) {
        using Output = Result<std::unique_ptr<ScriptModuleOperation>>;
        if (auto admitted = Admit(*impl_, program, policy, cancellation); admitted.HasError())
            return Output::Failure(admitted.ErrorValue());
        if (auto reference = PrepareReference(program->RequestsReference()); reference.HasError())
            return Output::Failure(reference.ErrorValue());
        auto context = Context(*impl_, *program, std::move(policy), cancellation);
        if (context.HasError())
            return Output::Failure(context.ErrorValue());
        auto admittedContext = std::move(context).Value();
        auto vm = Runtime(program->Runtime(), admittedContext);
        if (vm.HasError())
            return Output::Failure(vm.ErrorValue());
        auto state =
            std::make_unique<ScriptModuleOperation::Impl>(shared_from_this(), std::move(vm).Value(), std::move(admittedContext), program);
        ++impl_->activeOperations;
        state->counted = true;
        auto operation = std::unique_ptr<ScriptModuleOperation>{new ScriptModuleOperation{std::move(state)}};
        *std::ranges::find(impl_->operations, nullptr) = operation.get();
        if (auto started = operation->impl_->runtime->Start(program->Source()); started.HasError()) {
            SetScriptModuleFailure(operation->impl_->result, operation->impl_->emergency, started.ErrorValue(), {});
            operation->Cancel();
        }
        return Output::Success(std::move(operation));
    }
}  // namespace Horo::Extensions
