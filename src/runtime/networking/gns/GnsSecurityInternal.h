#pragma once
/** @file GnsSecurityInternal.h @brief Private native-root provisioning and generation-bound authority lease. */
#include "GnsTransportFactory.h"
#include "horo_security_bridge.h"

#include <mutex>
#include <optional>
#include <vector>

class ISteamNetworkingSockets;

namespace Horo::Network {
    class GnsTransport;

    /** @brief One revocable owner implements the four existing canonical authority contracts. */
    class GnsSecurity final : public ICertificateAuthority,
                              public IPeerVerificationAuthority,
                              public IPrivateKeyAuthority,
                              public IAdmissionChannelEvidence {
    public:
        explicit GnsSecurity(GnsNativeSecurityOptions options) : options_(std::move(options)) {}

        [[nodiscard]] bool Available() const noexcept override;
        [[nodiscard]] Result<PeerAuthenticationEvidence> Inspect(ConnectionHandle connection, NetworkOperationGeneration generation,
                                                                 std::uint64_t nowTick) override;
        [[nodiscard]] Result<CertificateVerificationResult> Verify(const CertificateVerificationRequest &request) override;
        [[nodiscard]] Result<PeerVerificationResult> Verify(const PeerVerificationRequest &request) override;
        [[nodiscard]] Result<SecureChannelHandoff> Bind(const SecureChannelBindingRequest &request) override;
        /** @brief Native-only preparation under transport lock; no issuer callback. @return Bounded public CSR. */
        [[nodiscard]] Result<std::vector<std::byte>> Prepare(ISteamNetworkingSockets &native, std::uint64_t &rootGeneration);
        /** @brief Invoke retained issuer outside every transport/native lock. @return Owned certificate or original failure. */
        [[nodiscard]] Result<Security::SecureBytes> Issue(std::span<const std::byte> request) const;
        /** @brief Attach only after selected-root certificate readiness. */
        void Attach(GnsTransport &transport) noexcept;
        /** @brief Revoke before taking transport mutex; provider lock order is lease then transport. */
        void Detach() noexcept;

    private:
        [[nodiscard]] bool Matches(const AuthenticationChallenge &challenge) const noexcept;
        [[nodiscard]] Result<SteamNetworkingSocketsLib::HoroNativeChannel> Read(const AuthenticationChallenge &challenge);
        GnsNativeSecurityOptions options_;
        mutable std::mutex mutex_;
        GnsTransport *transport_{};  // Revoked before native teardown; never retained by a callback.
    };

    /** @brief Preserve the closed native failure classification in existing typed network errors. */
    [[nodiscard]] Error GnsSecurityFailure(SteamNetworkingSocketsLib::HoroBridgeStatus status);
}  // namespace Horo::Network
