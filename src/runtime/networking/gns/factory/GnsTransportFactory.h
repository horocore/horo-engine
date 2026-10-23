#pragma once

/** @file GnsTransportFactory.h @brief Explicit host factory for the optional production transport. */

#include "Horo/Network/NetworkAdmissionController.h"
#include "Horo/Network/NetworkTransport.h"

#include <array>
#include <chrono>
#include <memory>

namespace Horo::Network {
    /**
     * @brief Creates the selected GNS adapter without initializing native sockets.
     * @return Unique transport or a typed unavailable/capacity error; never a Null substitute.
     */
    [[nodiscard]] Result<std::unique_ptr<INetworkTransport>> CreateGnsTransport();

    /** @brief Explicit embedding issuer; signs only the public native-generated CSR before any listener opens. */
    class IGnsCertificateIssuer {
    public:
        virtual ~IGnsCertificateIssuer() = default;
        /** @brief Return a bounded signed certificate without private-key fields.
         * @param request Public native CSR, borrowed only during this call.
         * @param deadline Explicit startup deadline; issuer must finish or cancel within it.
         * @return Securely owned certificate or original typed provider failure.
         * @details Runs outside native/Horo locks during startup, never NetworkPoll. Implementations
         * must honor the deadline; no callback may retain the request or destroy the calling owner.
         */
        [[nodiscard]] virtual Result<Security::SecureBytes> Issue(std::span<const std::byte> request,
                                                                  std::chrono::steady_clock::time_point deadline) = 0;
    };

    /** @brief Owned explicit native trust/provisioning selection; no default root, identity or issuer. */
    struct GnsNativeSecurityOptions final {
        std::array<std::byte, 32> trustedRoot{};
        std::uint32_t appId{};
        NetworkTrustPolicySnapshot trust{};
        NetworkTrustLevel peerTrust{NetworkTrustLevel::Count};
        NetworkTransportConfig transport{0, 0, 0}; /**< Exact inert candidate limits; initialization must match. */
        std::chrono::milliseconds provisioningBudget{};
        std::shared_ptr<IGnsCertificateIssuer> issuer;
    };

    /** @brief Four views of one revocable backend authority; credentials/proofs/entropy remain host injected. */
    struct GnsNativeAuthorities final {
        std::shared_ptr<ICertificateAuthority> certificates;
        std::shared_ptr<IPeerVerificationAuthority> peers;
        std::shared_ptr<IPrivateKeyAuthority> privateKeys;
        std::shared_ptr<IAdmissionChannelEvidence> channel;
    };

    /** @brief Unique transport plus its exact native authority owner, inert until transport Initialize provisions it. */
    struct GnsSecureTransportComposition final {
        std::unique_ptr<INetworkTransport> transport;
        GnsNativeAuthorities native;
    };

    /** @brief Select explicit native trust without opening sockets or invoking issuer callbacks.
     * @param options Deep-owned policy/root and retained bounded startup issuer.
     * @return Transport and the same native provider owner, or typed invalid/capacity failure.
     * @details Authorities are unavailable before successful initialization and after detach. Native
     * keys never leave GNS. Binding digests correlate public context; they are not authentication/exporters.
     */
    [[nodiscard]] Result<GnsSecureTransportComposition> CreateSecureGnsTransport(GnsNativeSecurityOptions options);
}  // namespace Horo::Network
