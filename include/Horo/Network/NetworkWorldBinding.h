#pragma once

/** @file NetworkWorldBinding.h @brief Bounded authenticated world-occurrence handoff; never an object authority grant. */

#include "Horo/Foundation/Sha256.h"
#include "Horo/Network/AuthenticationSessionAdapter.h"
#include "Horo/Network/HandshakeNegotiation.h"
#include "Horo/Network/MessageCodecRegistry.h"
#include "Horo/Network/ReplicationRoles.h"

#include <array>
#include <memory>
#include <span>

namespace Horo::Network {
    /** @brief Explicit invocation intent; transport qualification never publishes a gameplay world. */
    enum class NetworkWorldCompositionPurpose : std::uint8_t {
        Gameplay,
        TransportQualification,
        Count
    };

    /** @brief Closed world handoff vocabulary, scoped to one already authenticated server channel. */
    enum class NetworkWorldBindingKind : std::uint16_t {
        Initial,
        PrepareTravel,
        CommitTravel,
        Count
    };

    /** @brief Public identity metadata; accepting these bytes alone grants neither a world nor mutation permission. */
    struct NetworkWorldBindingRecord final {
        NetworkWorldBindingKind kind{NetworkWorldBindingKind::Count};
        NetworkSessionGeneration worldSession;
        ReplicationAuthorityEpoch remoteEpoch;
        std::uint64_t schemaFingerprint{};
        Sha256Digest expectedPriorBinding; /**< Zero for Initial; exact current binding digest for travel. */

        auto operator<=>(const NetworkWorldBindingRecord &) const noexcept = default;
    };

    /** @brief Host-selected immutable authorization and registry identities, captured before listening. */
    struct NetworkWorldBindingPolicy final {
        MessageIdentityDescriptor control;
        ProtocolFeatureId feature;
        NetworkPrincipalId expectedServer;
        CredentialProvenanceId expectedProvenance;
        NetworkCapabilityId authorityPublisher;
    };

    /** @brief Deep-owned values and the same immutable codec snapshot, captured by the actual host admission composition.
     * @details Capture validates policy against its exact required-feature declaration before creating this value.
     * The runtime revalidates every peer's canonical negotiated schema/features and principal at use time.
     */
    struct NetworkWorldBindingSetup final {
        NetworkWorldBindingPolicy policy;
        std::shared_ptr<const MessageCodecRegistry> codecs;
        std::uint64_t schemaFingerprint{};
        ChannelId controlChannel{};
        std::uint32_t maximumPeers{64};
        std::uint64_t maximumPendingTicks{5000};
    };

    inline constexpr std::size_t NetworkWorldBindingWireBytes = 60;
    using NetworkWorldBindingBytes = std::array<std::byte, NetworkWorldBindingWireBytes>;

    /** @brief Validate inert required-feature and exact codec inventory without invoking a provider.
     * @param policy Explicit selected server identity/provenance/capability and product control identity.
     * @param local Same compatibility declaration used by the canonical admission controller.
     * @param codecs Same immutable registry later used by the inbound dispatcher.
     * @return Success or a typed admission-policy error; no default identity is installed. */
    [[nodiscard]] Result<void> ValidateNetworkWorldBindingPolicy(const NetworkWorldBindingPolicy &policy,
                                                                 const HandshakeLocalDescriptor &local, const MessageCodecRegistry &codecs);

    /** @brief Encode the fixed big-endian v1 record without allocation.
     * @param record Complete occurrence and expected previous binding identity.
     * @return Exact bytes or a typed malformed-record error. */
    [[nodiscard]] Result<NetworkWorldBindingBytes> EncodeNetworkWorldBinding(const NetworkWorldBindingRecord &record);
    /** @brief Decode only the fixed closed representation; authentication is a separate mandatory boundary.
     * @param bytes Exact v1 payload, borrowed synchronously and never retained.
     * @return Valid public metadata or typed malformed/version error. */
    [[nodiscard]] Result<NetworkWorldBindingRecord> DecodeNetworkWorldBinding(std::span<const std::byte> bytes);
    /** @brief Hash domain-separated canonical occurrence/schema identity (independent of handoff phase) only; this is not authentication or
     * proof.
     * @param record Valid complete record.
     * @return Correlation digest or the original encoding error. */
    [[nodiscard]] Result<Sha256Digest> NetworkWorldBindingDigest(const NetworkWorldBindingRecord &record);
}  // namespace Horo::Network
