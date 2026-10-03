#pragma once

/**
 * @file ApplicationCapabilityRegistry.h
 * @brief Explicit composition-owned registry for versioned application capability providers.
 */

#include "Horo/Extensions/ExtensionCapabilityAdmission.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace Horo::Extensions {
    /** @brief Stable semantic version of one application capability contract. */
    struct ApplicationCapabilityVersion final {
        std::uint16_t major{}; /**< Breaking contract revision. */
        std::uint16_t minor{}; /**< Backward-compatible contract revision. */
        std::uint16_t patch{}; /**< Behavior-compatible correction revision. */

        [[nodiscard]] constexpr auto operator<=>(const ApplicationCapabilityVersion &) const noexcept = default;
    };

    /** @brief Closed version interval accepted by a capability consumer. */
    struct ApplicationCapabilityVersionRange final {
        ApplicationCapabilityVersion minimum{}; /**< Oldest accepted provider contract. */
        ApplicationCapabilityVersion maximum{}; /**< Newest accepted provider contract. */
    };

    /** @brief Exact module-to-provider ownership and generation selected by composition. */
    struct ApplicationCapabilityProviderIdentity final {
        std::string moduleId;       /**< Module that owns the provider publication. */
        std::string providerId;     /**< Stable provider identity within the host composition. */
        std::uint64_t generation{}; /**< Non-zero provider activation generation. */

        bool operator==(const ApplicationCapabilityProviderIdentity &) const noexcept = default;
    };

    /** @brief Immutable identity and ownership metadata for one provider publication. */
    struct ApplicationCapabilityProviderDescriptor final {
        ExtensionCapabilityId capability;               /**< Stable application capability identity. */
        ApplicationCapabilityVersion version{};         /**< Exact provider contract version. */
        ApplicationCapabilityProviderIdentity provider; /**< Explicit module/provider ownership and generation. */
    };

    struct ApplicationCapabilityRegistryState;
    struct ApplicationCapabilityProviderState;

    /** @brief Move-only registration whose lifetime controls provider discoverability. */
    class ApplicationCapabilityProviderRegistration final {
    public:
        ~ApplicationCapabilityProviderRegistration();
        ApplicationCapabilityProviderRegistration(const ApplicationCapabilityProviderRegistration &) = delete;
        ApplicationCapabilityProviderRegistration &operator=(const ApplicationCapabilityProviderRegistration &) = delete;
        ApplicationCapabilityProviderRegistration(ApplicationCapabilityProviderRegistration &&other) noexcept;
        ApplicationCapabilityProviderRegistration &operator=(ApplicationCapabilityProviderRegistration &&other) noexcept;

        /** @brief Idempotently removes this exact provider publication. */
        void Reset() noexcept;

        /** @brief Reports whether this registration still owns a live publication. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class ApplicationCapabilityRegistry;
        ApplicationCapabilityProviderRegistration(std::weak_ptr<ApplicationCapabilityRegistryState> registry,
                                                  std::shared_ptr<ApplicationCapabilityProviderState> provider) noexcept;

        std::weak_ptr<ApplicationCapabilityRegistryState> registry_;
        std::shared_ptr<ApplicationCapabilityProviderState> provider_;
    };

    /**
     * @brief Lease proving compatible provider resolution and caller capability admission.
     *
     * Capability-specific adapters retain this lease for the complete host-mediated call.
     * It exposes metadata only and never a concrete service, native handle, or global locator.
     */
    class ApplicationCapabilityProviderLease final {
    public:
        ApplicationCapabilityProviderLease(const ApplicationCapabilityProviderLease &) = delete;
        ApplicationCapabilityProviderLease &operator=(const ApplicationCapabilityProviderLease &) = delete;
        ApplicationCapabilityProviderLease(ApplicationCapabilityProviderLease &&) noexcept = default;
        ApplicationCapabilityProviderLease &operator=(ApplicationCapabilityProviderLease &&) noexcept = default;

        /** @brief Returns the exact provider publication selected for this call. */
        [[nodiscard]] const ApplicationCapabilityProviderDescriptor &Descriptor() const noexcept;

        /** @brief Returns the exact consumer activation that owns this admitted call. */
        [[nodiscard]] const ExtensionActivationIdentity &Consumer() const noexcept;

        /** @brief Returns whether both provider publication and consumer admission remain active. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend class ApplicationCapabilityRegistry;
        ApplicationCapabilityProviderLease(std::shared_ptr<const ApplicationCapabilityProviderState> provider,
                                           ExtensionCapabilityUseLease admission) noexcept;

        std::shared_ptr<const ApplicationCapabilityProviderState> provider_;
        ExtensionCapabilityUseLease admission_;
    };

    /** @brief Explicit host-owned registry of inert application capability provider metadata. */
    class ApplicationCapabilityRegistry final {
    public:
        static constexpr std::size_t MaximumProviders = 256; /**< Hard publication bound. */

        ApplicationCapabilityRegistry();
        ~ApplicationCapabilityRegistry();
        ApplicationCapabilityRegistry(const ApplicationCapabilityRegistry &) = delete;
        ApplicationCapabilityRegistry &operator=(const ApplicationCapabilityRegistry &) = delete;
        ApplicationCapabilityRegistry(ApplicationCapabilityRegistry &&) noexcept = default;
        ApplicationCapabilityRegistry &operator=(ApplicationCapabilityRegistry &&) noexcept = default;

        /**
         * @brief Publishes inert provider metadata from the application composition root.
         * @param descriptor Exact provider identity, version, and generation.
         * @return Lifetime registration, or a typed invalid/duplicate/capacity/shutdown failure.
         */
        [[nodiscard]] Result<ApplicationCapabilityProviderRegistration> Register(ApplicationCapabilityProviderDescriptor descriptor);

        /**
         * @brief Resolves an exact capability through the caller's admitted handle.
         * @param authority Admission handle naming the only capability that may be resolved.
         * @param versions Closed compatible provider-version interval.
         * @param extensionId Calling extension identity.
         * @param moduleId Calling module identity.
         * @param activationGeneration Calling activation generation.
         * @return Highest compatible provider lease, or explicit unavailable/version/admission failure.
         */
        [[nodiscard]] Result<ApplicationCapabilityProviderLease> Resolve(const ExtensionCapabilityHandle &authority,
                                                                         const ApplicationCapabilityVersionRange &versions,
                                                                         std::string_view extensionId, std::string_view moduleId,
                                                                         std::uint64_t activationGeneration) const;

        /** @brief Resolves only the named provider publication, never another compatible provider. */
        [[nodiscard]] Result<ApplicationCapabilityProviderLease> ResolveExact(const ExtensionCapabilityHandle &authority,
                                                                              const ApplicationCapabilityVersionRange &versions,
                                                                              const ApplicationCapabilityProviderIdentity &identity,
                                                                              std::string_view extensionId, std::string_view moduleId,
                                                                              std::uint64_t activationGeneration) const;

        /** @brief Idempotently closes registration and resolution and revokes all publications. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether the registry has entered terminal shutdown. */
        [[nodiscard]] bool IsShutdown() const noexcept;

    private:
        [[nodiscard]] ApplicationCapabilityRegistryState &MutableState() noexcept;

        std::shared_ptr<ApplicationCapabilityRegistryState> state_;
    };
}  // namespace Horo::Extensions
