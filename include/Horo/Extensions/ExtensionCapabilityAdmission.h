#pragma once

/**
 * @file ExtensionCapabilityAdmission.h
 * @brief Host-owned permission evaluation and activation-scoped capability grants.
 */

#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Extensions {
    /** @brief Stable identity of one permission in the host's sealed catalog. */
    struct ExtensionPermissionId final {
        std::string value; /**< Canonical lowercase dot-separated identity. */

        bool operator==(const ExtensionPermissionId &) const noexcept = default;
    };

    /** @brief Stable identity of one capability exposed by the application composition root. */
    struct ExtensionCapabilityId final {
        std::string value; /**< Canonical lowercase dot-separated identity. */

        bool operator==(const ExtensionCapabilityId &) const noexcept = default;
    };

    /** @brief One manifest-derived capability request and its complete permission dependency set. */
    struct ExtensionCapabilityRequest final {
        ExtensionCapabilityId capability;                       /**< Exact capability requested by a contribution. */
        std::vector<ExtensionPermissionId> requiredPermissions; /**< Permissions required before this capability may be granted. */
    };

    /**
     * @brief Immutable trust-policy evidence supplied by the application composition root.
     *
     * This value records policy results; it does not discover services or compute trust.
     * Every approved permission must also exist in the sealed known-permission catalog.
     */
    struct ExtensionAdmissionPolicy final {
        std::uint64_t revision{};                                 /**< Non-zero policy revision used for audit attribution. */
        std::vector<ExtensionPermissionId> knownPermissions;      /**< Sealed permission catalog for this host composition. */
        std::vector<ExtensionPermissionId> approvedPermissions;   /**< Exact locally or organizationally approved subset. */
        std::vector<ExtensionCapabilityId> availableCapabilities; /**< Capabilities explicitly exposed by this host composition. */
    };

    /** @brief Identity and activation generation requesting one atomic capability grant set. */
    struct ExtensionAdmissionRequest final {
        std::string extensionId;                              /**< Canonical owning extension identity. */
        std::string moduleId;                                 /**< Canonical owning module identity. */
        std::uint64_t activationGeneration{};                 /**< Non-zero generation invalidated by replacement or deactivation. */
        std::vector<ExtensionCapabilityRequest> capabilities; /**< Complete manifest-derived request set for this module. */
    };

    class ExtensionCapabilityAdmission;
    class ExtensionActivationLease;
    class ExtensionCapabilityUseLease;
    class ExtensionCapabilityHandle;
    struct ExtensionCapabilityAdmissionState;

    /** @brief Immutable identity of one admitted extension-module activation. */
    class ExtensionActivationIdentity final {
    public:
        /** @brief Returns the owning extension identity. */
        [[nodiscard]] const std::string &ExtensionId() const noexcept;

        /** @brief Returns the owning module identity. */
        [[nodiscard]] const std::string &ModuleId() const noexcept;

        /** @brief Returns the exact non-zero activation generation. */
        [[nodiscard]] std::uint64_t Generation() const noexcept;

    private:
        friend struct ExtensionCapabilityAdmissionState;
        friend class ExtensionActivationLease;
        friend class ExtensionCapabilityUseLease;
        friend class ExtensionCapabilityHandle;

        ExtensionActivationIdentity(std::string extensionId, std::string moduleId, std::uint64_t generation) noexcept;

        std::string extensionId_;
        std::string moduleId_;
        std::uint64_t generation_{};
    };

    /**
     * @brief Copyable proof that one extension activation remains live.
     *
     * The lease exposes activation metadata only. Revoking or destroying the
     * owning admission makes every retained lease unusable, which lets host
     * contexts bind their lifetime to the provider activation without exposing
     * the admission's internal state.
     */
    class ExtensionActivationLease final {
    public:
        ExtensionActivationLease(const ExtensionActivationLease &) = default;
        ExtensionActivationLease &operator=(const ExtensionActivationLease &) = default;
        ExtensionActivationLease(ExtensionActivationLease &&) noexcept = default;
        ExtensionActivationLease &operator=(ExtensionActivationLease &&) noexcept = default;

        /**
         * @brief Returns the exact extension-module activation named by this lease.
         * @return Activation evidence, or an empty identity for a moved-from lease.
         */
        [[nodiscard]] const ExtensionActivationIdentity &Activation() const noexcept;

        /** @brief Returns whether the owning admission still accepts activation-scoped work. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend class ExtensionCapabilityAdmission;

        explicit ExtensionActivationLease(std::shared_ptr<const ExtensionCapabilityAdmissionState> state);

        std::shared_ptr<const ExtensionCapabilityAdmissionState> state_;
    };

    /**
     * @brief Move-only guard proving a capability callback entered before revocation.
     *
     * Retain the guard for the complete callback. Revocation closes new admissions;
     * work admitted before that linearization point follows the owning host's drain policy.
     */
    class ExtensionCapabilityUseLease final {
    public:
        ExtensionCapabilityUseLease(const ExtensionCapabilityUseLease &) = delete;
        ExtensionCapabilityUseLease &operator=(const ExtensionCapabilityUseLease &) = delete;
        ExtensionCapabilityUseLease(ExtensionCapabilityUseLease &&) noexcept = default;
        ExtensionCapabilityUseLease &operator=(ExtensionCapabilityUseLease &&) noexcept = default;

        /** @brief Returns the exact capability admitted for this callback. */
        [[nodiscard]] const ExtensionCapabilityId &Capability() const noexcept;

        /**
         * @brief Returns the exact extension-module activation that owns this use lease.
         * @return Activation evidence, or an empty identity for a moved-from lease.
         */
        [[nodiscard]] const ExtensionActivationIdentity &Activation() const noexcept;

        /** @brief Returns whether the owning admission still accepts callback dispatch. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend class ExtensionCapabilityHandle;

        ExtensionCapabilityUseLease(std::shared_ptr<const ExtensionCapabilityAdmissionState> state, ExtensionCapabilityId capability);

        std::shared_ptr<const ExtensionCapabilityAdmissionState> state_;
        ExtensionCapabilityId capability_;
    };

    /**
     * @brief Copyable unforgeable evidence for one admitted capability.
     *
     * Consumers must call AcquireUse at their callback boundary and retain the returned
     * lease through dispatch. Admission destruction or explicit revocation makes every
     * later acquisition fail.
     */
    class ExtensionCapabilityHandle final {
    public:
        /** @brief Returns the exact admitted capability identity. */
        [[nodiscard]] const ExtensionCapabilityId &Capability() const noexcept;

        /**
         * @brief Returns the exact extension-module activation that owns this handle.
         * @return Activation evidence, or an empty identity for an inert handle.
         */
        [[nodiscard]] const ExtensionActivationIdentity &Activation() const noexcept;

        /**
         * @brief Atomically admits one capability callback for the exact owner generation.
         * @param extensionId Calling extension identity.
         * @param moduleId Calling module identity.
         * @param activationGeneration Calling activation generation.
         * @return Move-only callback guard, or a typed owner/revocation failure.
         */
        [[nodiscard]] Result<ExtensionCapabilityUseLease> AcquireUse(std::string_view extensionId, std::string_view moduleId,
                                                                     std::uint64_t activationGeneration) const;

        /** @brief Returns whether the owning admission still accepts callback dispatch. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend class ExtensionCapabilityAdmission;

        ExtensionCapabilityHandle(std::shared_ptr<const ExtensionCapabilityAdmissionState> state, ExtensionCapabilityId capability);

        std::shared_ptr<const ExtensionCapabilityAdmissionState> state_;
        ExtensionCapabilityId capability_;
    };

    /**
     * @brief Move-only atomic grant set scoped to one extension module activation.
     *
     * Evaluate validates the entire policy and request before publishing any handle.
     * Destruction revokes all retained handles. The class is safe to revoke while
     * other threads acquire callback-use leases.
     */
    class ExtensionCapabilityAdmission final {
    public:
        ~ExtensionCapabilityAdmission();

        ExtensionCapabilityAdmission(const ExtensionCapabilityAdmission &) = delete;
        ExtensionCapabilityAdmission &operator=(const ExtensionCapabilityAdmission &) = delete;
        ExtensionCapabilityAdmission(ExtensionCapabilityAdmission &&other) noexcept;
        ExtensionCapabilityAdmission &operator=(ExtensionCapabilityAdmission &&other) noexcept;

        /**
         * @brief Evaluates one complete module request against explicit host policy.
         * @param request Complete manifest-derived request for one module activation.
         * @param policy Host-composed permission approval and capability availability evidence.
         * @return Atomic admission, or a typed fail-closed error with no grant set.
         */
        [[nodiscard]] static Result<ExtensionCapabilityAdmission> Evaluate(const ExtensionAdmissionRequest &request,
                                                                           const ExtensionAdmissionPolicy &policy);

        /**
         * @brief Returns a handle only for an exactly declared and admitted capability.
         * @param capability Exact stable capability identity.
         * @return Activation-scoped handle, or explicit unavailable/revoked failure.
         */
        [[nodiscard]] Result<ExtensionCapabilityHandle> Grant(const ExtensionCapabilityId &capability) const;

        /** @brief Closes callback admission for every handle; safe repeatedly and across threads. */
        void Revoke() const noexcept;

        /** @brief Returns the immutable policy revision that produced this admission. */
        [[nodiscard]] std::uint64_t PolicyRevision() const noexcept;

        /**
         * @brief Returns a copyable lease tied to this exact activation generation.
         * @return Activation lease; admission revocation makes it unusable.
         */
        [[nodiscard]] ExtensionActivationLease ActivationLease() const;

        /** @brief Returns admitted capabilities in deterministic identity order. */
        [[nodiscard]] std::span<const ExtensionCapabilityId> Capabilities() const noexcept;

    private:
        ExtensionCapabilityAdmission(std::shared_ptr<ExtensionCapabilityAdmissionState> state,
                                     std::vector<ExtensionCapabilityId> capabilities) noexcept;

        std::shared_ptr<ExtensionCapabilityAdmissionState> state_;
        std::vector<ExtensionCapabilityId> capabilities_;
    };
}  // namespace Horo::Extensions
