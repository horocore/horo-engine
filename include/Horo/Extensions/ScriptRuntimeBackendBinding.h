#pragma once
/** @file ScriptRuntimeBackendBinding.h @brief Host-issued typed execution ports over existing exact backend imports. */
#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ScriptCapabilityContext.h"

#include <atomic>
#include <concepts>
#include <functional>

namespace Horo::Extensions {
    /** @brief Actual composition evidence, never supplied by a VM or package callback. */
    struct ScriptRuntimeBackendBindingInput final {
        ResolvedExtensionServiceImport import;
        ExtensionCapabilityHandle consumerCapability;
        ExtensionCapabilityHandle providerCapability;
        BackendServiceCodeLease code;
        BackendServiceDescriptor service;
        std::shared_ptr<ApplicationCapabilityRegistry> capabilities;
        std::shared_ptr<BackendServiceRegistry> services;
        std::shared_ptr<ScriptInvocationProviderRegistration> provider;
        ScriptExportDescriptorSnapshotPtr descriptors;
        std::string apiId;
        ScriptExportVersionRange versions;
        ScriptCapabilityProfile profile{ScriptCapabilityProfile::Count};
        std::vector<std::string> functions;
        std::shared_ptr<const std::atomic<bool>>
            hostClosed; /**< Aliases the actual composition admission flag when present; never a mirrored liveness claim. */

        ScriptRuntimeBackendBindingInput(ResolvedExtensionServiceImport resolved, ExtensionCapabilityHandle consumer,
                                         ExtensionCapabilityHandle source, BackendServiceCodeLease executable)
            : import(std::move(resolved)), consumerCapability(std::move(consumer)), providerCapability(std::move(source)),
              code(std::move(executable)) {}
    };

    namespace Detail {
        /** @brief Engine-owned erased execution shape; no service lookup registry or VM callbacks. */
        class ScriptRuntimeBackendPort {
        public:
            virtual ~ScriptRuntimeBackendPort() = default;
            virtual const ScriptRuntimeBackendBindingInput &Input() const noexcept = 0;
            virtual Result<void> Probe() const = 0;
            virtual Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &, const CancellationToken &) const = 0;
        };

        Result<void> ValidateScriptRuntimeBackendInput(const ScriptRuntimeBackendBindingInput &input);
        Result<void> ValidateScriptRuntimeBackendDescriptor(const ScriptRuntimeBackendBindingInput &, const BackendServiceDescriptor *);

        template <class Service, class Operation> class TypedScriptRuntimeBackendPort final : public ScriptRuntimeBackendPort {
        public:
            TypedScriptRuntimeBackendPort(ScriptRuntimeBackendBindingInput input, Operation operation)
                : input_(std::move(input)), operation_(std::move(operation)) {}

            const ScriptRuntimeBackendBindingInput &Input() const noexcept override {
                return input_;
            }

            Result<void> Probe() const override {
                auto call = Resolve();
                if (call.HasError())
                    return Result<void>::Failure(call.ErrorValue());
                return Result<void>::Success();
            }

            Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &request, const CancellationToken &cancel) const override {
                auto call = Resolve();
                if (call.HasError())
                    return Result<ScriptCallResult>::Failure(call.ErrorValue());
                return std::move(call).Value().Invoke(operation_, request, cancel);
            }

        private:
            Result<BackendServiceCall<Service>> Resolve() const {
                const auto &owner = input_.consumerCapability.Activation();
                auto authority =
                    input_.capabilities->ResolveExact(input_.consumerCapability, {input_.service.version, input_.service.version},
                                                      input_.service.provider, owner.ExtensionId(), owner.ModuleId(), owner.Generation());
                if (authority.HasError())
                    return Result<BackendServiceCall<Service>>::Failure(authority.ErrorValue());
                auto imported = input_.services->BindImport(std::move(authority).Value(), input_.import);
                if (imported.HasError())
                    return Result<BackendServiceCall<Service>>::Failure(imported.ErrorValue());
                auto resolved = input_.services->ResolveImported<Service>(std::move(imported).Value());
                if (resolved.HasError())
                    return resolved;
                auto actual = ValidateScriptRuntimeBackendDescriptor(input_, resolved.Value().Descriptor());
                if (actual.HasError())
                    return Result<BackendServiceCall<Service>>::Failure(actual.ErrorValue());
                return resolved;
            }

            ScriptRuntimeBackendBindingInput input_;
            Operation operation_;
        };
    }  // namespace Detail
    class ScriptRuntimeBackendBinding;
    class ScriptModuleContext;
    class ScriptModuleBindingCache;
    struct ScriptModuleContextAccess;
    template <class Service, class Operation>
    Result<std::shared_ptr<const ScriptRuntimeBackendBinding>> BindScriptRuntimeBackend(ScriptRuntimeBackendBindingInput, Operation);

    /** @brief Immutable exact-provider execution port; constructing one requires a real typed backend binding. */
    class ScriptRuntimeBackendBinding final {
    public:
        [[nodiscard]] const ScriptExportDescriptor &Descriptor() const noexcept;
        [[nodiscard]] const ResolvedExtensionServiceImport &Resolution() const noexcept;
        [[nodiscard]] bool IsUsable() const noexcept;
        [[nodiscard]] bool AllowsFunction(std::string_view id) const noexcept;

    private:
        friend class ScriptModuleContext;
        friend class ScriptModuleBindingCache;
        friend struct ScriptModuleContextAccess;
        friend struct ScriptModuleNativeBridgeAccess;
        [[nodiscard]] const ScriptRuntimeBackendBindingInput &Evidence() const noexcept;
        [[nodiscard]] ScriptCapabilityImport Import() const;
        [[nodiscard]] Result<ScriptInvocationController> Begin(const ScriptCapabilityBinding &, ScriptInvocationRequest) const;
        [[nodiscard]] Result<ScriptCallResult> Invoke(const ScriptInvocationRequest &, const CancellationToken &) const;
        [[nodiscard]] Result<ScriptHandle> IssueHandle(const ScriptCapabilityBinding &, std::string type) const;
        template <class Service, class Operation>
        friend Result<std::shared_ptr<const ScriptRuntimeBackendBinding>> BindScriptRuntimeBackend(ScriptRuntimeBackendBindingInput,
                                                                                                   Operation);

        explicit ScriptRuntimeBackendBinding(std::shared_ptr<const Detail::ScriptRuntimeBackendPort> port) : port_(std::move(port)) {}

        std::shared_ptr<const Detail::ScriptRuntimeBackendPort> port_;
    };

    /** @brief Binds the real registered concrete type once; no second same-ID registration is created. */
    template <class Service, class Operation>
    Result<std::shared_ptr<const ScriptRuntimeBackendBinding>> BindScriptRuntimeBackend(ScriptRuntimeBackendBindingInput input,
                                                                                        Operation operation) {
        static_assert(std::is_member_function_pointer_v<Operation>);
        static_assert(
            std::same_as<std::invoke_result_t<Operation, Service &, const ScriptInvocationRequest &, const BackendServiceCallContext &>,
                         Result<ScriptCallResult>>);
        using Output = Result<std::shared_ptr<const ScriptRuntimeBackendBinding>>;
        auto valid = Detail::ValidateScriptRuntimeBackendInput(input);
        if (valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto port = std::make_shared<Detail::TypedScriptRuntimeBackendPort<Service, Operation>>(std::move(input), std::move(operation));
        auto actual = port->Probe();
        if (actual.HasError())
            return Output::Failure(actual.ErrorValue());
        return Output::Success(std::shared_ptr<const ScriptRuntimeBackendBinding>{new ScriptRuntimeBackendBinding{std::move(port)}});
    }
}  // namespace Horo::Extensions
