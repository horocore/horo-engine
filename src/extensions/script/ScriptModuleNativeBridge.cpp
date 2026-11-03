#include "ScriptModuleNativeBridge.h"

namespace Horo::Extensions {
    namespace {
        class NativePort final : public Detail::ScriptRuntimeBackendPort {
        public:
            NativePort(ScriptRuntimeBackendBindingInput input, std::shared_ptr<const ModuleScriptNativePort> native)
                : input_(std::move(input)), native_(std::move(native)) {}

            const ScriptRuntimeBackendBindingInput &Input() const noexcept override {
                return input_;
            }

            Result<void> Probe() const override {
                return native_->Probe();
            }

            Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &request, const CancellationToken &cancel) const override {
                return native_->Invoke(request, cancel);
            }

        private:
            ScriptRuntimeBackendBindingInput input_;
            std::shared_ptr<const ModuleScriptNativePort> native_;
        };

        ScriptRuntimeBackendBindingInput Input(const ModuleScriptHostAuthority &host, const ModuleScriptNativeEvidence &actual,
                                               ScriptCapabilityProfile profile) {
            ScriptRuntimeBackendBindingInput input{actual.import, actual.consumer, actual.provider, actual.code};
            input.service = actual.descriptor;
            input.capabilities = host.Capabilities();
            input.services = host.Services();
            input.provider = actual.invocation;
            input.descriptors = actual.descriptors;
            input.apiId = actual.api;
            input.versions = actual.descriptors->Find(actual.api)->compatibility;
            input.profile = profile;
            input.functions = actual.functions;
            input.hostClosed = actual.closed;
            return input;
        }
    }  // namespace

    struct ScriptModuleNativeBridgeAccess final {
        static Result<std::shared_ptr<const ScriptRuntimeBackendBinding>> Bind(const ModuleScriptHostAuthority &host,
                                                                               const std::shared_ptr<const ModuleScriptNativePort> &native,
                                                                               ScriptCapabilityProfile profile) {
            using Output = Result<std::shared_ptr<const ScriptRuntimeBackendBinding>>;
            auto input = Input(host, native->Evidence(), profile);
            auto valid = Detail::ValidateScriptRuntimeBackendInput(input);
            if (valid.HasError())
                return Output::Failure(valid.ErrorValue());
            auto live = native->Probe();
            if (live.HasError())
                return Output::Failure(live.ErrorValue());
            auto port = std::make_shared<NativePort>(std::move(input), native);
            return Output::Success(std::shared_ptr<const ScriptRuntimeBackendBinding>{new ScriptRuntimeBackendBinding{std::move(port)}});
        }
    };

    Result<ScriptModuleImport> BindModuleScriptHostImport(const ModuleScriptHostAuthority &host, std::string_view package,
                                                          std::string_view module, std::string_view id, std::string alias,
                                                          ScriptCapabilityProfile profile) {
        using Output = Result<ScriptModuleImport>;
        auto resolved = host.Resolve(package, module, id);
        if (resolved.HasError())
            return Output::Failure(resolved.ErrorValue());
        if (!resolved.Value().Port())
            return MakeUnavailableScriptModuleImport(std::move(alias), resolved.Value().Resolution());
        auto port = ScriptModuleNativeBridgeAccess::Bind(host, resolved.Value().Port(), profile);
        if (port.HasError())
            return Output::Failure(port.ErrorValue());
        return BindScriptModuleImport(std::move(alias), std::move(port).Value());
    }

    Result<ScriptModuleImport> BindNativeModuleScriptImport(NativeExtensionServiceHost &native, std::string_view package,
                                                            std::string_view module, std::string_view id, std::string alias,
                                                            ScriptCapabilityProfile profile) {
        auto host = ModuleScriptHostAuthority::RetainState(native);
        if (host.HasError())
            return Result<ScriptModuleImport>::Failure(host.ErrorValue());
        return BindModuleScriptHostImport(*host.Value(), package, module, id, std::move(alias), profile);
    }

    Result<ScriptModuleImport> BindHeadlessModuleScriptImport(HeadlessExtensionHost &native, std::string_view package,
                                                              std::string_view module, std::string_view id, std::string alias,
                                                              ScriptCapabilityProfile profile) {
        auto host = ModuleScriptHostAuthority::Headless(native, false);
        if (host.HasError())
            return Result<ScriptModuleImport>::Failure(host.ErrorValue());
        return BindModuleScriptHostImport(*host.Value(), package, module, id, std::move(alias), profile);
    }
}  // namespace Horo::Extensions
