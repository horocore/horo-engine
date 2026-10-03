#pragma once

/** @file ScriptCapabilityContext.h
 * @brief Host-admitted script imports, scoped bindings and safe diagnostic identity.
 */
#include "Horo/Extensions/ScriptInvocation.h"

#include <atomic>
#include <string_view>

namespace Horo::Extensions {
    struct ScriptCapabilityContextState;
    struct ScriptCapabilityBindingState;

    /** @brief Closed execution profiles; tooling never inherits scene authority. */
    enum class ScriptCapabilityProfile : std::uint8_t {
        Tooling,
        Gameplay,
        Count
    };
    /** @brief Host lifecycle boundaries that independently revoke script bindings. */
    enum class ScriptCapabilityScopeKind : std::uint8_t {
        Project,
        Runtime,
        Scene,
        Operation,
        Count
    };

    /** @brief Copyable observer of a host-owned lifecycle scope. */
    class ScriptCapabilityScopeLease final {
    public:
        /** @brief Reports whether the owning scope is live. @return False after teardown. */
        [[nodiscard]] bool IsUsable() const noexcept;
        /** @brief Returns the scope boundary. @return Immutable scope kind. */
        [[nodiscard]] ScriptCapabilityScopeKind Kind() const noexcept;

    private:
        friend class ScriptCapabilityScope;
        ScriptCapabilityScopeLease(ScriptCapabilityScopeKind kind, std::shared_ptr<const std::atomic<bool>> active);
        ScriptCapabilityScopeKind kind_;
        std::shared_ptr<const std::atomic<bool>> active_;
    };

    /**
     * @brief Host teardown authority; destruction revokes every copied lease across threads.
     * Atomic liveness protects admission against cross-thread teardown. Metadata is immutable;
     * revocation is permanent, and moving/destroying the owner is never concurrent with owner use.
     */
    class ScriptCapabilityScope final {
    public:
        /** @brief Creates a lifecycle scope. @param kind Host boundary owned by this scope. */
        explicit ScriptCapabilityScope(ScriptCapabilityScopeKind kind);
        ~ScriptCapabilityScope() noexcept;
        ScriptCapabilityScope(const ScriptCapabilityScope &) = delete;
        ScriptCapabilityScope &operator=(const ScriptCapabilityScope &) = delete;
        /** @brief Closes scope admission permanently; safe from any thread. */
        void Revoke() const noexcept;
        /** @brief Returns a revocable observer. @return Lease without native resource access. */
        [[nodiscard]] ScriptCapabilityScopeLease Lease() const;

    private:
        ScriptCapabilityScopeKind kind_;
        std::shared_ptr<std::atomic<bool>> active_;
    };

    /** @brief Declared import plus its exact host-resolved descriptor and admitted provider grant. */
    struct ScriptCapabilityImport final {
        std::string apiId;
        ScriptExportVersionRange versions;
        ScriptExportDescriptorSnapshotPtr descriptors;
        ExtensionCapabilityHandle capability;
        ScriptInvocationProviderLease provider; /**< Exact registered provider; reset permanently expires this binding. */
        ScriptCapabilityProfile profile{ScriptCapabilityProfile::Count}; /**< Host-approved operation profile. */
        std::vector<std::string> functions; /**< Exact approved operations; an empty allowlist grants no calls. */
    };

    /**
     * @brief Immutable host policy evidence; all capability layers are intersected before binding.
     * No layer is inferred from installation, source location or runtime participation.
     */
    struct ScriptCapabilityContextDescriptor final {
        std::string scriptId; /**< Canonical identity only; never source, path or arbitrary script text. */
        std::uint64_t policyRevision{};
        ScriptCapabilityProfile profile{ScriptCapabilityProfile::Count};
        bool packageVerified{};
        bool packageTrusted{};
        bool scriptTrusted{};
        bool runtimeCompatible{};
        std::vector<ExtensionCapabilityId> packageCapabilities;
        std::vector<ExtensionCapabilityId> scriptCapabilities;
        std::vector<ExtensionCapabilityId> projectCapabilities;
        std::vector<ExtensionCapabilityId> contextCapabilities;
        std::vector<ScriptCapabilityScopeLease> scopes;
        std::vector<ScriptCapabilityImport> imports; /**< Required imports; denial rejects the entire context. */
    };

    /** @brief Copyable unforgeable import binding; retained copies fail after any owning scope ends. */
    class ScriptCapabilityBinding final {
    public:
        /** @brief Reports whether all binding owners still admit work. @return False after revocation. */
        [[nodiscard]] bool IsUsable() const noexcept;
        /**
         * @brief Admits a call through the host invocation ABI with the exact resolved descriptor.
         * @param provider Live provider registration matching the admitted activation generation.
         * @param request Function, owned arguments, timeout and affinity; descriptor/API fields are host supplied.
         * @param operationId Host operation correlation; zero selects the invocation identity.
         * @return Controller or a typed denied/revoked/lifecycle failure.
         */
        [[nodiscard]] Result<ScriptInvocationController> Begin(const ScriptInvocationProviderRegistration &provider,
                                                               ScriptInvocationRequest request, std::uint64_t operationId = 0) const;

    private:
        friend class ScriptCapabilityContext;
        explicit ScriptCapabilityBinding(std::shared_ptr<const ScriptCapabilityBindingState> state);
        std::shared_ptr<const ScriptCapabilityBindingState> state_;
    };

    /**
     * @brief Move-only host context; reset suppresses VM delivery before cancelling pending work.
     * Create, Bind and Drain belong to the VM owner thread; Reset may run during host teardown.
     * Move/destruction must not race methods on the same owner object. Copied binding metadata is
     * immutable, and atomic active state protects calls against permanent host revocation.
     */
    class ScriptCapabilityContext final {
    public:
        ~ScriptCapabilityContext() noexcept;
        ScriptCapabilityContext(const ScriptCapabilityContext &) = delete;
        ScriptCapabilityContext &operator=(const ScriptCapabilityContext &) = delete;
        ScriptCapabilityContext(ScriptCapabilityContext &&other) noexcept;
        ScriptCapabilityContext &operator=(ScriptCapabilityContext &&other) noexcept;
        /**
         * @brief Atomically validates all required imports before publishing a context.
         * @param registry Explicit shared host registry retained for every binding lifetime.
         * @param descriptor Host-composed declaration, trust, policy and scope evidence.
         * @param invocation Invocation resource bounds and parent cancellation.
         * @return Context or a typed invalid/denied/capacity failure; no partial bindings escape.
         */
        [[nodiscard]] static Result<ScriptCapabilityContext> Create(std::shared_ptr<ScriptInvocationRegistry> registry,
                                                                    ScriptCapabilityContextDescriptor descriptor,
                                                                    ScriptInvocationContextDescriptor invocation = {});
        /** @brief Resolves only a declared import. @param apiId Exact identity. @return Binding or typed denial. */
        [[nodiscard]] Result<ScriptCapabilityBinding> Bind(std::string_view apiId) const;
        /** @brief Revokes all binding copies and cancels work without calling a VM or provider. */
        void Reset() const noexcept;
        /** @brief Drains completion on the owner safe point. @param maximumEvents Finite drain bound. @return Events or revocation error.
         */
        [[nodiscard]] Result<std::vector<ScriptInvocationEvent>> Drain(std::size_t maximumEvents = 64) const;

    private:
        explicit ScriptCapabilityContext(std::shared_ptr<ScriptCapabilityContextState> state);
        std::shared_ptr<ScriptCapabilityContextState> state_;
    };
}  // namespace Horo::Extensions
