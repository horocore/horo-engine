#pragma once

/**
 * @file ModuleHost.h
 * @brief Host-owned module registration, activation, cancellation, drainage, and shutdown.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Configuration.h"
#include "Horo/Foundation/ErrorCodeRegistry.h"
#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace Horo {
    class ModuleActivationContext;
    class ModuleCallbackGate;

    /** @brief Owned, inert settings registered explicitly by the composition root for one module. */
    struct ModuleConfigurationContribution {
        ModuleId module;
        std::string ownerPrefix;
        std::vector<SettingDescriptor> settings;
        std::vector<EnvironmentVariableBinding> environmentBindings;
    };

    /** @brief Host-approved dependency bindings owned by the composition root. */
    class IDependencyBindings {
    public:
        virtual ~IDependencyBindings() = default;

        IDependencyBindings(const IDependencyBindings &) = delete;
        IDependencyBindings &operator=(const IDependencyBindings &) = delete;

    protected:
        IDependencyBindings() = default;
        IDependencyBindings(IDependencyBindings &&) = default;
        IDependencyBindings &operator=(IDependencyBindings &&) = default;
    };

    /** @brief Explicit observable state of one module registration lifetime. */
    enum class ModuleLifecycleState : std::uint8_t {
        Registered,
        Activating,
        Active,
        CancellationRequested,
        Draining,
        Stopped,
        Failed,
    };

    /**
     * @brief Move-only admission lease keeping one asynchronous module callback drainable.
     *
     * A lease may outlive the activation callback, but must be released after observing
     * cancellation. Module shutdown closes admission, requests cancellation, and waits
     * for every outstanding lease before invoking the module drain callback.
     */
    class ModuleCallbackLease {
    public:
        ~ModuleCallbackLease();

        ModuleCallbackLease(const ModuleCallbackLease &) = delete;
        ModuleCallbackLease &operator=(const ModuleCallbackLease &) = delete;
        ModuleCallbackLease(ModuleCallbackLease &&other) noexcept;
        ModuleCallbackLease &operator=(ModuleCallbackLease &&other) noexcept;

        /** @brief Returns the activation-scoped bindings borrow guarded by this lease. */
        [[nodiscard]] const IDependencyBindings *Bindings() const noexcept;

        /** @brief Returns the module-owned cancellation token associated with this lease. */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;

    private:
        friend class ModuleActivationContext;

        ModuleCallbackLease(std::shared_ptr<ModuleCallbackGate> gate, const IDependencyBindings *bindings,
                            CancellationToken cancellation) noexcept;
        void Release() noexcept;

        std::shared_ptr<ModuleCallbackGate> m_gate;
        const IDependencyBindings *m_bindings{};
        CancellationToken m_cancellation;
    };

    /** @brief A module instance created by an activation callback and owned by the host. */
    class IModuleInstance {
    public:
        virtual ~IModuleInstance() = default;

        IModuleInstance(const IModuleInstance &) = delete;
        IModuleInstance &operator=(const IModuleInstance &) = delete;

    protected:
        IModuleInstance() = default;
        IModuleInstance(IModuleInstance &&) = default;
        IModuleInstance &operator=(IModuleInstance &&) = default;
    };

    /**
     * @brief Capabilities a composition root grants to one activating module.
     *
     * The context is constructed by the host for exactly one activation call. It is
     * not a service locator: the composition root decides which approved dependency
     * bindings each module receives, and modules never discover ambient state.
     */
    class ModuleActivationContext {
    public:
        /** @brief Opaque carrier for host-approved bindings; owned by the composition root. */
        using DependencyBindings = const IDependencyBindings *;

        ModuleActivationContext(ModuleId module, DependencyBindings bindings);
        ~ModuleActivationContext();

        ModuleActivationContext(const ModuleActivationContext &) = delete;
        ModuleActivationContext &operator=(const ModuleActivationContext &) = delete;

        [[nodiscard]] const ModuleId &Module() const noexcept;

        /**
         * @brief Returns the host-approved dependency bindings supplied for activation.
         * @return Opaque bindings owned by the composition root; may be null.
         */
        [[nodiscard]] DependencyBindings Bindings() const noexcept;

        /**
         * @brief Returns the module-owned cooperative cancellation token.
         * @return Token that becomes cancelled before callback drainage begins.
         */
        [[nodiscard]] CancellationToken Cancellation() const noexcept;

        /**
         * @brief Attempts to admit one asynchronous callback into this module lifetime.
         * @return A move-only lease, or std::nullopt after shutdown closes admission.
         *
         * Callbacks retain the returned lease for their full use of activation-scoped
         * bindings. Shutdown waits for all admitted leases before releasing dependencies.
         */
        [[nodiscard]] std::optional<ModuleCallbackLease> AcquireCallbackLease() const;

        /**
         * @brief Stores an instance the activated module owns until deactivation.
         * @param instance Non-null module-owned instance.
         * @return Failure when @p instance is null; success otherwise.
         */
        [[nodiscard]] Result<void> AttachInstance(std::unique_ptr<IModuleInstance> instance);

    private:
        friend class ModuleHost;

        void RequestShutdown() const noexcept;

        /**
         * @brief Blocks until all admitted callback leases are released.
         *
         * Module callbacks are expected to cooperate by checking CancellationToken and
         * releasing leases promptly upon observing cancellation. Drainage never releases
         * activation-scoped dependencies while a lease can still borrow them.
         */
        void DrainCallbacks() const noexcept;

        ModuleId m_module;
        DependencyBindings m_bindings;
        std::unique_ptr<IModuleInstance> m_instance;
        CancellationSource m_cancellation;
        std::shared_ptr<ModuleCallbackGate> m_callbackGate;
    };

    /** @brief One activated module: its identity, lifecycle entry points, and attached instance. */
    struct ActiveModule {
        ModuleId id;                                      /**< Activated module identity. */
        ModuleDrainCallback drain{nullptr};               /**< Owned-work drainage entry point. */
        ModuleDeactivateCallback deactivate{nullptr};     /**< Matching deactivation entry point. */
        std::unique_ptr<ModuleActivationContext> context; /**< Activation context owning the attached instance. */
    };

    /**
     * @brief Registers inert descriptors explicitly at composition time, validates them,
     *        activates modules in deterministic order, and rolls back on failure.
     *
     * A failed activation deactivates every module started by that activation call in
     * reverse order, leaving no partially active set from the failed call and preserving
     * modules active before it. Registration never invokes callbacks; only
     * ActivateRegistered does. Host control methods are single-threaded by contract;
     * admitted asynchronous callbacks coordinate shutdown through callback leases.
     */
    class ModuleHost {
    public:
        ModuleHost() = default;

        /** @brief Runs idempotent cancellation, drainage, and reverse-order teardown as a final safety net. */
        ~ModuleHost();

        ModuleHost(const ModuleHost &) = delete;
        ModuleHost &operator=(const ModuleHost &) = delete;

        /**
         * @brief Registers one inert descriptor without invoking any callback.
         * @param descriptor Descriptor copied into the host's pending registration set.
         * @return Failure when identity or metadata is malformed locally, or already registered.
         */
        [[nodiscard]] Result<void> Register(const ModuleDescriptor &descriptor);

        /** @brief Atomically registers a module and its owned settings; rejects conflicts before activation. */
        [[nodiscard]] Result<void> Register(const ModuleDescriptor &descriptor, const ModuleConfigurationContribution &contribution);

        /** @brief Builds a mutable schema from active modules in stable module/key order. */
        [[nodiscard]] Result<ConfigurationSchema> BuildConfigurationSchema() const;

        /** @brief Returns active environment bindings in stable module/variable order. */
        [[nodiscard]] std::vector<EnvironmentVariableBinding> ConfigurationEnvironmentBindings() const;

        /**
         * @brief Validates the full registration graph and activates modules in validated order.
         * @param bindings Host-approved dependency bindings handed to every activation callback.
         * @return Success with the count activated by this call, or a typed validation/
         *         activation failure with only this call's started modules deactivated
         *         in reverse order.
         */
        [[nodiscard]] Result<std::size_t> ActivateRegistered(ModuleActivationContext::DependencyBindings bindings);

        /**
         * @brief Cancels all active modules, drains callbacks, deactivates in reverse order,
         *        and terminally stops pending registrations; safe repeatedly.
         */
        void DeactivateAll() noexcept;

        /** @brief Returns whether any module is currently active. */
        [[nodiscard]] bool HasActiveModules() const noexcept;

        /**
         * @brief Returns the latest lifecycle state for one identity in this host.
         * @param id Module identity previously registered with this host.
         * @return Current or terminal state, or std::nullopt for an unknown identity.
         */
        [[nodiscard]] std::optional<ModuleLifecycleState> StateOf(const ModuleId &id) const noexcept;

        /**
         * @brief Returns the latest immutable registry published by a successful activation.
         * @return Shared registry snapshot, or null before the first successful activation.
         *
         * Existing snapshots remain valid when a later incremental activation publishes an
         * extended registry. Failed activation never replaces the last successful snapshot.
         */
        [[nodiscard]] std::shared_ptr<const ErrorCodeRegistry> ErrorCodes() const noexcept;

    private:
        struct ModuleStateRecord {
            ModuleId id;
            ModuleLifecycleState state{ModuleLifecycleState::Registered};
        };

        void Transition(const ModuleId &id, ModuleLifecycleState state) noexcept;
        void RequestCancellationFrom(std::size_t base) noexcept;
        void DeactivateFrom(std::size_t base) noexcept;
        void DeactivateActiveModule(ActiveModule &active) noexcept;
        void StopUnactivatedRegistered() noexcept;
        void RollbackActivation(const ModuleId &failedId, std::unique_ptr<ModuleActivationContext> failedContext,
                                std::size_t base) noexcept;

        std::vector<ModuleDescriptor> m_registered;
        std::vector<ModuleConfigurationContribution> m_configurationContributions;
        std::vector<ActiveModule> m_active;
        std::vector<ModuleStateRecord> m_states;
        std::shared_ptr<const ErrorCodeRegistry> m_errorCodes;
    };
}  // namespace Horo
