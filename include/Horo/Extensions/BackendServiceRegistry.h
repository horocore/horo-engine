#pragma once
#include "Horo/Extensions/ExtensionRetirement.h"

/**
 * @file BackendServiceRegistry.h
 * @brief Typed, host-owned invocation boundary for backend-only service contributions.
 */

#include "Horo/Extensions/ApplicationCapabilityRegistry.h"
#include "Horo/Extensions/ExtensionModuleResolution.h"
#include "Horo/Foundation/CancellationToken.h"

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Extensions {
    struct BackendServiceProviderState;
    struct BackendServiceRegistryState;
    class BackendServiceCallAdmission;
    class BackendServiceImportBinding;
    class BackendServiceRegistry;

    namespace Detail {
        [[nodiscard]] Result<BackendServiceCallAdmission> BeginBackendServiceCall(
            const std::shared_ptr<BackendServiceProviderState> &provider, const CancellationToken &caller);
    }

    /** @brief Stable identity of one backend-neutral callable service export. */
    struct BackendServiceId final {
        std::string value;
        bool operator==(const BackendServiceId &) const noexcept = default;
    };

    /** @brief Stable identity of the typed C++ contract implemented by a service export. */
    struct BackendServiceContractId final {
        std::string value;
        bool operator==(const BackendServiceContractId &) const noexcept = default;
    };

    /** @brief Thread rule enforced before provider code is entered. */
    enum class BackendServiceThreadRule : std::uint8_t {
        AnyThread,
        ProviderOwnerThread,
    };

    /** @brief Observable result of a bounded provider-retirement request. */
    enum class BackendServiceRetirementDisposition : std::uint8_t {
        ShutdownComplete,                /**< Provider shutdown completed exactly once. */
        DeferredUntilCallExit,           /**< A re-entrant call guard owns finalization. */
        OwnerThreadFinalizationRequired, /**< A drained provider awaits its recorded owner thread. */
        RestartRequired,                 /**< Deadline expired; the live provider remains retained. */
    };

    /** @brief Finite lifecycle policy for one backend service registry. */
    struct BackendServiceRegistryConfig final {
        std::chrono::milliseconds drainDeadline{std::chrono::seconds(5)}; /**< Maximum synchronous cooperative drain wait. */
    };

    /** @brief Opaque shared ownership that keeps provider executable code mapped through service destruction. */
    class BackendServiceCodeLease final {
    public:
        /**
         * @brief Retains one host-owned executable/module lifetime object.
         * @tparam Owner Concrete lifetime owner, such as a loaded module lease.
         * @param owner Non-null shared lifetime owner.
         * @return Opaque lease validated during service registration.
         */
        template <typename Owner> [[nodiscard]] static BackendServiceCodeLease Retain(std::shared_ptr<Owner> owner) noexcept {
            return BackendServiceCodeLease{std::move(owner)};
        }

    private:
        friend class BackendServiceRegistry;

        explicit BackendServiceCodeLease(std::shared_ptr<const void> owner) noexcept : owner_(std::move(owner)) {}

        std::shared_ptr<const void> owner_;
    };

    namespace Detail {
        /** @brief Ordering-safe ownership of one service object and the code required to destroy it. */
        struct BackendServiceOwnedImplementation final {
            BackendServiceCodeLease codeLease;
            std::shared_ptr<void> service;
        };
    }  // namespace Detail

    /** @brief Immutable service identity, capability binding, and invocation policy. */
    struct BackendServiceDescriptor final {
        BackendServiceId serviceId;                                               /**< Stable exported service identity. */
        BackendServiceContractId contractId;                                      /**< Stable typed callable contract identity. */
        ExtensionCapabilityId capability;                                         /**< Application capability authorizing calls. */
        ApplicationCapabilityVersion version{};                                   /**< Exact service contract version. */
        ApplicationCapabilityProviderIdentity provider;                           /**< Exact module/provider owner and generation. */
        BackendServiceThreadRule threadRule{BackendServiceThreadRule::AnyThread}; /**< Enforced call-thread policy. */
    };

    /** @brief Cancellation and attribution supplied to one admitted typed service operation. */
    class BackendServiceCallContext final {
    public:
        /** @brief Returns the exact provider generation executing the operation. */
        [[nodiscard]] const BackendServiceDescriptor &Provider() const noexcept;

        /** @brief Reports cancellation requested by either the caller or provider lifecycle. */
        [[nodiscard]] bool IsCancellationRequested() const noexcept;

    private:
        friend class BackendServiceCallAdmission;
        friend Result<BackendServiceCallAdmission> Detail::BeginBackendServiceCall(const std::shared_ptr<BackendServiceProviderState> &,
                                                                                   const CancellationToken &);
        BackendServiceCallContext(const BackendServiceDescriptor &provider, CancellationToken caller,
                                  CancellationToken providerCancellation) noexcept;

        const BackendServiceDescriptor *provider_{};
        CancellationToken caller_;
        CancellationToken providerCancellation_;
    };

    /** @brief Move-only admission guard retaining one active provider operation. */
    class BackendServiceCallAdmission final {
    public:
        ~BackendServiceCallAdmission();
        BackendServiceCallAdmission(const BackendServiceCallAdmission &) = delete;
        BackendServiceCallAdmission &operator=(const BackendServiceCallAdmission &) = delete;
        BackendServiceCallAdmission(BackendServiceCallAdmission &&other) noexcept = default;
        BackendServiceCallAdmission &operator=(BackendServiceCallAdmission &&other) noexcept = delete;

        /** @brief Returns the operation context valid for this admitted call. */
        [[nodiscard]] const BackendServiceCallContext &Context() const noexcept;

    private:
        friend class BackendServiceRegistry;
        template <typename Service> friend class BackendServiceCall;
        friend Result<BackendServiceCallAdmission> Detail::BeginBackendServiceCall(const std::shared_ptr<BackendServiceProviderState> &,
                                                                                   const CancellationToken &);
        BackendServiceCallAdmission(std::shared_ptr<BackendServiceProviderState> provider, BackendServiceCallContext context) noexcept;
        void Reset() noexcept;

        std::shared_ptr<BackendServiceProviderState> provider_;
        BackendServiceCallContext context_;
    };

    namespace Detail {
        template <typename Service> inline constexpr std::byte BackendServiceTypeTag{};

        [[nodiscard]] void *BackendServiceObject(const std::shared_ptr<BackendServiceProviderState> &provider) noexcept;
        [[nodiscard]] Error AttributeBackendServiceError(const BackendServiceDescriptor &provider, Error cause);
        [[nodiscard]] Error BackendServiceCancellationError(const BackendServiceDescriptor &provider);
        [[nodiscard]] Error BackendServiceCancellationError(const ApplicationCapabilityProviderDescriptor &provider);
        [[nodiscard]] Error BackendServiceAuthorityUnavailableError();
        [[nodiscard]] Error BackendServiceConsumedCallError();
        [[nodiscard]] Error BackendServiceImportError(const ResolvedExtensionServiceImport &binding, const ErrorCodeDescriptor &descriptor,
                                                      std::string_view reason,
                                                      const ApplicationCapabilityProviderDescriptor *provider = nullptr);
        [[nodiscard]] Error AttributeBackendServiceImportError(const ResolvedExtensionServiceImport &binding, Error error,
                                                               std::string_view reason = {},
                                                               const ApplicationCapabilityProviderDescriptor *provider = nullptr);
    }  // namespace Detail

    /** @brief One-shot typed invocation handle bound to an admitted application capability lease. */
    template <typename Service> class BackendServiceCall final {
    public:
        BackendServiceCall(const BackendServiceCall &) = delete;
        BackendServiceCall &operator=(const BackendServiceCall &) = delete;
        BackendServiceCall(BackendServiceCall &&) noexcept = default;
        BackendServiceCall &operator=(BackendServiceCall &&) noexcept = default;

        /**
         * @brief Invokes one mutable typed service operation exactly once.
         * @tparam Operation Mutable or const member operation owned by the service contract.
         * @tparam Request Operation request type owned by the service contract.
         * @param operation Typed member operation to invoke.
         * @param request Immutable request passed to the provider.
         * @param cancellation Caller-owned cooperative cancellation.
         * @return Typed response or attributed provider, cancellation, lifecycle, or thread failure.
         */
        template <typename Operation, typename Request>
            requires std::invocable<Operation, Service &, const Request &, const BackendServiceCallContext &>
        [[nodiscard]] auto Invoke(Operation operation, const Request &request, CancellationToken cancellation = {})
            && -> std::invoke_result_t<Operation, Service &, const Request &, const BackendServiceCallContext &> {
            return std::move(*this).InvokeImpl(operation, request, std::move(cancellation));
        }

    private:
        friend class BackendServiceRegistry;

        BackendServiceCall(std::shared_ptr<BackendServiceProviderState> provider, ApplicationCapabilityProviderLease authority) noexcept
            : provider_(std::move(provider)), authority_(std::move(authority)) {}

        template <typename Operation, typename Request>
        [[nodiscard]] auto InvokeImpl(Operation operation, const Request &request, CancellationToken cancellation)
            && -> std::invoke_result_t<Operation, Service &, const Request &, const BackendServiceCallContext &> {
            using OperationResult = std::invoke_result_t<Operation, Service &, const Request &, const BackendServiceCallContext &>;
            const auto provider = std::exchange(provider_, {});
            if (provider == nullptr)
                return OperationResult::Failure(Detail::BackendServiceConsumedCallError());
            if (!authority_.IsUsable())
                return OperationResult::Failure(Detail::BackendServiceCancellationError(authority_.Descriptor()));
            auto admitted = Detail::BeginBackendServiceCall(provider, cancellation);
            if (admitted.HasError())
                return OperationResult::Failure(admitted.ErrorValue());
            BackendServiceCallAdmission call = std::move(admitted).Value();
            auto *service = static_cast<Service *>(Detail::BackendServiceObject(provider));
            try {
                OperationResult result = std::invoke(operation, *service, request, call.Context());
                if (call.Context().IsCancellationRequested())
                    return OperationResult::Failure(Detail::BackendServiceCancellationError(call.Context().Provider()));
                if (result.HasError())
                    return OperationResult::Failure(Detail::AttributeBackendServiceError(call.Context().Provider(), result.ErrorValue()));
                return result;
            } catch (...) {
                return OperationResult::Failure(Detail::AttributeBackendServiceError(call.Context().Provider(), {}));
            }
        }

        std::shared_ptr<BackendServiceProviderState> provider_;
        ApplicationCapabilityProviderLease authority_;
    };

    /** @brief Move-only registration owning discoverability, cancellation, drainage, and service shutdown. */
    class BackendServiceRegistration final {
    public:
        ~BackendServiceRegistration();
        BackendServiceRegistration(const BackendServiceRegistration &) = delete;
        BackendServiceRegistration &operator=(const BackendServiceRegistration &) = delete;
        BackendServiceRegistration(BackendServiceRegistration &&other) noexcept = default;
        BackendServiceRegistration &operator=(BackendServiceRegistration &&other) noexcept = delete;

        /**
         * @brief Revokes future calls, cancels and drains active work, then shuts down the service exactly once.
         * @return Completed, deferred owner-thread/self finalization, or restart-required retained retirement.
         */
        [[nodiscard]] BackendServiceRetirementDisposition Reset() noexcept;

        /** @brief Reports whether this registration still owns a discoverable service. */
        [[nodiscard]] bool IsRegistered() const noexcept;
        /**
         * @brief Binds service publication and its admitted operations/code closure to extension retirement.
         * @param retirement Owning package's retirement controller with verified module code already bound.
         * @return False for invalid, repeated, closed, off-owner-lane or already executing registration.
         * @details Bind on the registration's owner lane before exposing the service to callers. This logically
         * const handle operation mutates shared provider state; it does not relax owner-lane requirements.
         */
        [[nodiscard]] bool AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement) const;

    private:
        friend class BackendServiceRegistry;
        BackendServiceRegistration(std::shared_ptr<BackendServiceRegistryState> registry,
                                   std::shared_ptr<BackendServiceProviderState> provider) noexcept;

        std::shared_ptr<BackendServiceRegistryState> registry_;
        std::shared_ptr<BackendServiceProviderState> provider_;
    };

    /** @brief Move-only host-created proof of one admitted static import bound to an exact live provider generation. */
    class BackendServiceImportBinding final {
    public:
        BackendServiceImportBinding(const BackendServiceImportBinding &) = delete;
        BackendServiceImportBinding &operator=(const BackendServiceImportBinding &) = delete;
        BackendServiceImportBinding(BackendServiceImportBinding &&) noexcept = default;
        BackendServiceImportBinding &operator=(BackendServiceImportBinding &&) noexcept = default;

    private:
        friend class BackendServiceRegistry;

        BackendServiceImportBinding(std::shared_ptr<BackendServiceProviderState> provider, ApplicationCapabilityProviderLease authority,
                                    ResolvedExtensionServiceImport import) noexcept;

        std::shared_ptr<BackendServiceProviderState> provider_;
        ApplicationCapabilityProviderLease authority_;
        ResolvedExtensionServiceImport import_;
    };

    /** @brief Explicit host-owned registry for typed backend-only service contributions. */
    class BackendServiceRegistry final {
    public:
        static constexpr std::size_t MaximumServices = 256; /**< Hard publication bound. */

        explicit BackendServiceRegistry(BackendServiceRegistryConfig config = {});
        ~BackendServiceRegistry();
        BackendServiceRegistry(const BackendServiceRegistry &) = delete;
        BackendServiceRegistry &operator=(const BackendServiceRegistry &) = delete;
        BackendServiceRegistry(BackendServiceRegistry &&) noexcept = delete;
        BackendServiceRegistry &operator=(BackendServiceRegistry &&other) noexcept = delete;

        /**
         * @brief Publishes a typed service implementation without invoking provider code.
         * @tparam Service Contract type with a noexcept `Shutdown()` lifecycle method.
         * @param descriptor Exact service, capability, provider, version, generation, and thread policy.
         * @param service Provider implementation whose sole ownership transfers to the registry.
         * @param codeLease Shared host-owned lease keeping the implementation's executable code mapped.
         * @return Lifetime registration or a typed invalid, duplicate, capacity, or shutdown failure.
         */
        template <typename Service>
            requires requires(Service &service) {
                { service.Shutdown() } noexcept -> std::same_as<void>;
            }
        [[nodiscard]] Result<BackendServiceRegistration> Register(BackendServiceDescriptor descriptor, std::unique_ptr<Service> service,
                                                                  BackendServiceCodeLease codeLease) {
            Detail::BackendServiceOwnedImplementation implementation{std::move(codeLease), std::shared_ptr<void>{std::move(service)}};
            return RegisterErased(std::move(descriptor), std::move(implementation), &Detail::BackendServiceTypeTag<Service>,
                                  [](void *object) noexcept {
                static_cast<Service *>(object)->Shutdown();
            });
        }

        /**
         * @brief Resolves one exact typed service for a single host-mediated call.
         * @param authority Compatible application capability/provider lease for the caller.
         * @param serviceId Exact service export identity requested by the caller.
         * @param contractId Exact typed contract expected by the caller.
         * @return One-shot typed call handle or a typed unavailable, mismatch, or shutdown failure.
         */
        template <typename Service>
        [[nodiscard]] Result<BackendServiceCall<Service>> Resolve(ApplicationCapabilityProviderLease authority,
                                                                  const BackendServiceId &serviceId,
                                                                  const BackendServiceContractId &contractId) const {
            if (!authority.IsUsable())
                return Result<BackendServiceCall<Service>>::Failure(Detail::BackendServiceAuthorityUnavailableError());
            auto resolved = ResolveErased(authority.Descriptor(), serviceId, contractId, &Detail::BackendServiceTypeTag<Service>);
            if (resolved.HasError())
                return Result<BackendServiceCall<Service>>::Failure(resolved.ErrorValue());
            return Result<BackendServiceCall<Service>>::Success(
                BackendServiceCall<Service>{std::move(resolved).Value(), std::move(authority)});
        }

        /**
         * @brief Binds one immutable declared import to an exact admitted consumer and live provider generation.
         * @param authority Provider lease acquired through the consumer activation's admitted capability.
         * @param import Unforgeable static import resolution emitted by `ResolveExtensionModules`.
         * @return Move-only host binding, or an attributed unavailable, ownership, version, or generation failure.
         */
        [[nodiscard]] Result<BackendServiceImportBinding> BindImport(ApplicationCapabilityProviderLease authority,
                                                                     const ResolvedExtensionServiceImport &import) const;

        /**
         * @brief Converts one exact host-created import binding into a one-shot typed call.
         * @tparam Service Concrete host adapter contract expected by the consumer.
         * @param binding Move-only binding returned by `BindImport`; consumed exactly once.
         * @return One-shot typed call, or an attributed revocation, replacement, type, or shutdown failure.
         */
        template <typename Service>
        [[nodiscard]] Result<BackendServiceCall<Service>> ResolveImported(BackendServiceImportBinding binding) const {
            auto validated = ValidateImportedBinding(binding, &Detail::BackendServiceTypeTag<Service>);
            if (validated.HasError())
                return Result<BackendServiceCall<Service>>::Failure(validated.ErrorValue());
            return Result<BackendServiceCall<Service>>::Success(
                BackendServiceCall<Service>{std::move(binding.provider_), std::move(binding.authority_)});
        }

        /**
         * @brief Idempotently revokes, cancels, drains, and shuts down every service.
         * @return Aggregate completed, deferred, or restart-required disposition.
         */
        [[nodiscard]] BackendServiceRetirementDisposition BeginShutdown() noexcept;

        /**
         * @brief Finalizes drained retired providers whose owner is the calling thread.
         * @return Aggregate disposition; RestartRequired remains sticky after a deadline breach.
         */
        [[nodiscard]] BackendServiceRetirementDisposition FinalizeRetiredOnOwnerThread() noexcept;

        /** @brief Reports whether registration and resolution are terminally closed. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        using ShutdownFunction = void (*)(void *) noexcept;
        [[nodiscard]] Result<BackendServiceRegistration> RegisterErased(BackendServiceDescriptor descriptor,
                                                                        Detail::BackendServiceOwnedImplementation implementation,
                                                                        const void *typeTag, ShutdownFunction shutdown);
        [[nodiscard]] Result<std::shared_ptr<BackendServiceProviderState>> ResolveErased(
            const ApplicationCapabilityProviderDescriptor &authority, const BackendServiceId &serviceId,
            const BackendServiceContractId &contractId, const void *typeTag) const;
        [[nodiscard]] Result<void> ValidateImportedBinding(const BackendServiceImportBinding &binding, const void *typeTag) const;

        std::shared_ptr<BackendServiceRegistryState> state_;
    };
}  // namespace Horo::Extensions
