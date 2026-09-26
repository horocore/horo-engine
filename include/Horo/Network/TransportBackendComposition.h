#pragma once

/**
 * @file TransportBackendComposition.h
 * @brief Explicit host-owned registration, selection, and activation of optional network transports.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Network/NetworkTargetCapabilities.h"
#include "Horo/Network/TransportBackendInstance.h"
#include "Horo/Network/TransportCapabilities.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Network {
    /** @brief Stable host-visible backend identity, not a native SDK identifier. */
    struct TransportBackendId final {
        std::string value;
        [[nodiscard]] bool IsValid() const noexcept;
        bool operator==(const TransportBackendId &) const noexcept = default;
    };

    /** @brief Composition lifecycle, independent of a backend's connection lifecycle. */
    enum class TransportBackendCompositionState : std::uint8_t {
        Configuring,
        Sealed,
        Selected,
        Active,
        Cancelling,
        Closed,
    };

    /** @brief Exact observable state of one requested backend identity. */
    struct TransportBackendStatus final {
        bool installed{};     /**< Descriptor and factory explicitly registered by this host. */
        bool hostSupported{}; /**< Host platform/policy admits this installed backend. */
        bool configured{};    /**< Host has supplied a complete backend configuration. */
        bool selected{};      /**< This exact backend was selected without fallback. */
        bool active{};        /**< Its factory succeeded and the registry uniquely owns the instance. */
    };

    /** @brief Inert registration evidence; no factory or native handle escapes. */
    struct TransportBackendEvidence final {
        TransportBackendStatus status{};
        TransportCapabilities capabilities{};
    };

    /** @brief Host-private binding between a Horo provider ID and an installed backend registration. */
    struct TransportTargetBinding final {
        NetworkTransportProviderId provider{};
        TransportBackendId backend{};
    };

    /** @brief Host-supplied factory; only Activate may invoke it. */
    using TransportBackendFactory = std::function<Result<TransportBackendInstance>()>;

    /** @brief Inert installed-backend facts and factory binding from approved host composition. */
    struct TransportBackendDescriptor final {
        TransportBackendId id;
        bool hostSupported{};
        bool configured{};
        TransportCapabilities capabilities{};
        TransportBackendFactory factory;
    };

    /**
     * @brief Single-threaded composition owner; no global discovery or implicit default backend.
     *
     * The host explicitly links and registers only the backend modules admitted by its profile.
     * Registration and selection never invoke a factory. The active instance is cancelled then
     * shut down before its factory/module code can be unloaded. The host must keep factory code
     * loaded through registry destruction; dynamic providers require an external code lease.
     */
    class TransportBackendComposition final {
    public:
        static constexpr std::size_t MaximumBackends = 16;

        TransportBackendComposition() = default;
        ~TransportBackendComposition() noexcept;
        TransportBackendComposition(const TransportBackendComposition &) = delete;
        TransportBackendComposition &operator=(const TransportBackendComposition &) = delete;

        /** @brief Register one validated installed backend without executing it. @return Typed invalid, duplicate, capacity or lifecycle
         * result. */
        [[nodiscard]] Result<void> Register(TransportBackendDescriptor descriptor);
        /** @brief Close registration. @return Success; repeated seals are idempotent. */
        [[nodiscard]] Result<void> Seal() noexcept;
        /** @brief Select exactly one installed, supported, configured backend. @return No fallback on an unavailable request. */
        [[nodiscard]] Result<void> Select(const TransportBackendId &id);
        /** @brief Invoke only the selected factory and own its non-null result. @return Failure preserves selected but inactive state. */
        [[nodiscard]] Result<void> Activate();
        /** @brief Observe exact installed/support/configuration/selection/activation facts. @return Typed malformed-ID failure. */
        [[nodiscard]] Result<TransportBackendStatus> Status(const TransportBackendId &id) const;
        /** @brief Read inert status and capability evidence without activating the backend. @return Empty status for absent IDs. */
        [[nodiscard]] Result<TransportBackendEvidence> Evidence(const TransportBackendId &id) const;
        /** @brief Request cooperative cancellation and close new activation admission. Idempotent. */
        void BeginCancellation() noexcept;
        /** @brief Shut down and release the active instance, then close the composition. Idempotent. */
        void Shutdown() noexcept;

        /** @brief Return the exact composition lifecycle. */
        [[nodiscard]] TransportBackendCompositionState Lifecycle() const noexcept {
            return state_;
        }

        /** @brief Return installed IDs in host registration order, without exposing executable factories. */
        [[nodiscard]] std::vector<TransportBackendId> InstalledIds() const;

    private:
        std::vector<TransportBackendDescriptor> descriptors_;
        std::optional<TransportBackendId> selected_;
        TransportBackendInstance active_;
        TransportBackendCompositionState state_{TransportBackendCompositionState::Configuring};
    };

    /**
     * @brief Capture actual registered backend facts for target assessment after sealing composition.
     * @param composition Explicit host-owned composition; no factory is invoked.
     * @param revision Host-published revision bumped after any replacement.
     * @param platform Exact host platform, not a renderer or device tier.
     * @param supportedRoles Roles independently admitted by this host.
     * @param protocol Host-owned protocol/schema support.
     * @param bindings Explicit unique provider-to-backend mappings.
     * @return Bounded target host facts or a typed invalid, capacity or lifecycle error.
     */
    [[nodiscard]] Result<NetworkTargetHostFacts> CaptureNetworkTargetHostFacts(
        const TransportBackendComposition &composition, NetworkHostCapabilityRevision revision, NetworkTargetPlatform platform,
        NetworkProjectRoleSet supportedRoles, NetworkProjectProtocolPolicy protocol, std::span<const TransportTargetBinding> bindings);
}  // namespace Horo::Network
