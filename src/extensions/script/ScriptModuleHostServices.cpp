#include "ScriptModuleHostServicesInternal.h"
#include "ScriptModuleProjection.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        bool ReferenceAllowed(const ScriptModuleOperationPolicy &policy) {
            const ExtensionCapabilityId capability{"horo.script.reference"};
            return std::ranges::find(policy.projectCapabilities, capability) != policy.projectCapabilities.end() &&
                   std::ranges::find(policy.contextCapabilities, capability) != policy.contextCapabilities.end();
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
                auto reference = state.reference->Bind("horo.reference");
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
