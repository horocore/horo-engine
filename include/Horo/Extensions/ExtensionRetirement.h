#pragma once

/**
 * @file ExtensionRetirement.h
 * @brief Attributed extension retirement, contribution revocation and executable-work leases.
 */

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Host-owned reason a module must remain executable. */
    enum class ExtensionLeaseKind {
        Callback,
        Job,
        Resource,
        UiSurface,
        HostService
    };

    /** @brief Nonblocking shutdown result; Draining requires releasing the attributed work or restarting. */
    enum class ExtensionRetirementDisposition {
        Active,
        Draining,
        Complete,
        RestartRequired
    };

    /** @brief Owned diagnostic identifying one outstanding executable-work lease. */
    struct ExtensionOutstandingLease final {
        std::string moduleId;
        std::string subject;
        ExtensionLeaseKind kind{ExtensionLeaseKind::Resource};
    };

    /** @brief Inspection result; shutdown never waits indefinitely or forcibly unloads a provider. */
    struct ExtensionRetirementReport final {
        std::string extensionId;
        ExtensionRetirementDisposition disposition{ExtensionRetirementDisposition::Active};
        std::vector<ExtensionOutstandingLease> outstanding;
    };

    /**
     * @brief Host-owned revocation adapter for one contribution publication.
     * @details Revoke closes new work and removes discovery/UI/subscription ownership. It must be idempotent,
     * must not wait indefinitely for module work, and must retain admitted work's objects through executable-work
     * leases. A service adapter may use its existing bounded cancellation/drain deadline and require restart on failure.
     */
    class IExtensionRetirementContribution {
    public:
        virtual ~IExtensionRetirementContribution() = default;
        /** @brief Closes publication admission without destroying objects still owned by admitted work. */
        virtual void Revoke() noexcept = 0;
    };

    struct ExtensionRetirementState;

    /** @brief Shared work token; its final release destroys provider state before removing its diagnostic. */
    class ExtensionExecutableLease final {
    public:
        /** @brief Construction authority reserved to the retirement admission controller. */
        class ConstructionKey final {
            friend class ExtensionRetirement;
            ConstructionKey() = default;
        };

        /**
         * @brief Constructs an unadmitted token before acquiring the state mutex.
         * @param key Controller-only construction authority.
         * @param owner Provider owner retained through admission or rollback.
         */
        ExtensionExecutableLease(ConstructionKey key, std::shared_ptr<void> owner);
        ~ExtensionExecutableLease();
        ExtensionExecutableLease(const ExtensionExecutableLease &) = delete;
        ExtensionExecutableLease &operator=(const ExtensionExecutableLease &) = delete;

    private:
        friend class ExtensionRetirement;
        std::shared_ptr<ExtensionRetirementState> state_;
        std::size_t id_{};
        std::shared_ptr<void> moduleCode_;
        std::shared_ptr<void> owner_;
    };

    /**
     * @brief Coordinates one extension's owner-lane retirement and thread-safe executable-work admission.
     * @details Construction and contribution registration belong to host composition. Modules are supplied in
     * dependency-first activation order. BeginRetirement revokes dependent modules first and then returns;
     * outstanding work keeps its provider objects and complete executable dependency closure alive.
     * Lease acquisition/release and Inspect are thread-safe. RegisterContribution and BeginRetirement belong
     * to the same host owner lane. This API does not authorize live package replacement or force unload.
     * Const handles share the same synchronized controller state; const does not imply immutable lifecycle state
     * or relax owner-lane requirements. Only the controller can construct executable-work tokens.
     */
    class ExtensionRetirement final {
    public:
        /**
         * @brief Creates a bounded retirement owner for an already resolved module graph.
         * @param extensionId Exact owning extension identity.
         * @param dependencyFirstModules Validated module identities in dependency-first activation order.
         * @throws std::bad_alloc Allocation failure; no contributions have been admitted or published.
         */
        ExtensionRetirement(std::string extensionId, std::vector<std::string> dependencyFirstModules);
        /** @brief Closes admission and revokes publications on the host owner lane. */
        ~ExtensionRetirement();
        ExtensionRetirement(const ExtensionRetirement &) = delete;
        ExtensionRetirement &operator=(const ExtensionRetirement &) = delete;
        ExtensionRetirement(ExtensionRetirement &&) = delete;
        ExtensionRetirement &operator=(ExtensionRetirement &&) = delete;
        /**
         * @brief Binds verified native code and its provider dependency closure before module registration.
         * @param moduleId Exact module from the resolved graph.
         * @param codeOwner Strong executable dependency-closure owner supplied by the native loader.
         * @return False for unknown, already bound, retired or null ownership.
         * @details Only weak ownership is stored here; every admitted work/publication token takes its own
         * strong pin. Host composition must not replace a module's bound code within one activation.
         */
        [[nodiscard]] bool BindModuleCode(std::string_view moduleId, const std::shared_ptr<void> &codeOwner) const;
        /**
         * @brief Admits attributed work only before retirement begins.
         * @param moduleId Exact resolved owning module identity.
         * @param kind Work category for shutdown diagnostics.
         * @param subject Stable contribution/job/resource identity.
         * @param owner Strong provider object and executable dependency-closure owner; null is rejected.
         * @return Shared work token, or null for closed admission, unknown identity or exhausted bounds.
         */
        [[nodiscard]] std::shared_ptr<ExtensionExecutableLease> Acquire(std::string_view moduleId, ExtensionLeaseKind kind,
                                                                        std::string subject, std::shared_ptr<void> owner) const;
        /**
         * @brief Retains one host revocation owner while the extension is active.
         * @param moduleId Exact resolved owning module identity.
         * @param contribution Host adapter whose Revoke closes admission and removes the contribution.
         * @return False for unknown modules, closed admission, null owner or exhausted bounds.
         */
        [[nodiscard]] bool RegisterContribution(std::string_view moduleId,
                                                std::shared_ptr<IExtensionRetirementContribution> contribution) const;
        /**
         * @brief Closes work admission and revokes contributions dependent-first; repeated calls are safe.
         * @return Attributed drain/restart state after revocation.
         * @throws std::bad_alloc Report allocation failure; admission remains closed and revocation is complete.
         */
        [[nodiscard]] ExtensionRetirementReport BeginRetirement() const;
        /**
         * @brief Allocation-free shutdown safety path; revokes every publication even when report allocation cannot succeed.
         * @details Owner-lane only. Reentrant calls observe retirement already started and do not repeat revocation.
         */
        void CloseAdmission() const noexcept;
        /**
         * @brief Returns current attributed work; Draining means release listed owners or restart, never force unload.
         * @return Owned snapshot of the extension disposition and outstanding work identities.
         * @throws std::bad_alloc Snapshot allocation failure; retirement state is unchanged.
         */
        [[nodiscard]] ExtensionRetirementReport Inspect() const;
        /**
         * @brief Allocation-free owner-lane finalization predicate.
         * @return False while work/revocation remains or a sticky restart-required failure was recorded.
         */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Records a sticky provider drain/retained-code failure; process restart is the only recovery. */
        void RequireRestart() const noexcept;

    private:
        std::shared_ptr<ExtensionRetirementState> state_;
    };
}  // namespace Horo::Extensions
