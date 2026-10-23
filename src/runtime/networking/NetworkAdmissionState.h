#pragma once
/** @file NetworkAdmissionState.h @brief Private fixed peer ownership and canonical admission continuation state. */
#include "NetworkAdmissionWire.h"

#include <optional>
#include <thread>
#include <vector>

namespace Horo::Network {
    struct NetworkAdmissionController::Impl final {
        struct Peer final {
            /** @brief Exact once-only transport cleanup outcome; independent from the first canonical terminal reason. */
            enum class Retirement : std::uint8_t {
                Live,
                Closing,
                Closed,
                Rejected,
                CallbackFailed
            };
            ConnectionHandle connection;
            NetworkOperationGeneration generation;
            AdmissionEndpoint endpoint{AdmissionEndpoint::Count};
            PeerSessionDeadlines deadlines;
            std::shared_ptr<PeerSessionLifecycle> lifecycle;
            std::optional<HandshakeNegotiator> negotiation;
            std::optional<AuthenticationSessionAdapter> authentication;
            AuthenticationNonce localNonce{};
            AuthenticationNonce remoteNonce{};
            AuthenticationChallenge challenge;
            std::optional<AdmissionWire::Challenge> peerChallenge;
            std::optional<ProofIssuanceAuthorization> issuance;
            std::optional<TransportSelectionEvidence> expectedPeerTransport;
            std::uint32_t nextSequence{1};
            std::uint32_t lastSequence{};
            bool ledgerReserved{};
            Retirement retirement{Retirement::Live};
            bool proofIssued{};
            bool proposalAcknowledged{};
            bool proposalReceived{};
        };

        Impl(INetworkTransport &transport, InboundMessageDispatcher &dispatcher, std::shared_ptr<const MessageCodecRegistry> codecs,
             const NetworkAdmissionPolicy &policy, NetworkAdmissionProviders providers, AdmissionProtection protection,
             std::weak_ptr<INetworkAdmissionObserver> observer);
        [[nodiscard]] Result<void> CheckTick(std::uint64_t nowTick) const;
        [[nodiscard]] Result<void> ValidatePolicy();
        [[nodiscard]] Result<void> ValidateBounds() const;
        [[nodiscard]] Result<void> ValidateControls() const;
        [[nodiscard]] Result<void> DispatchControl(Peer &peer, const MessageEnvelopePayloadView &frame, std::uint64_t nowTick);
        [[nodiscard]] Result<void> InitializePeer(std::optional<Peer> &slot, const NetworkTransportEvent &event, AdmissionEndpoint endpoint,
                                                  const PeerSessionDeadlines &deadlines);
        [[nodiscard]] Result<void> Connect(const NetworkTransportEvent &event, std::uint64_t nowTick);
        [[nodiscard]] Peer *Find(ConnectionHandle connection) noexcept;
        [[nodiscard]] Result<void> AbortProjection(Peer &peer, std::uint64_t nowTick);
        [[nodiscard]] Result<void> ProjectPrepared(Peer &peer, std::uint64_t nowTick);
        [[nodiscard]] Result<void> Fail(Peer &peer, NetworkFailureKind failure, std::uint64_t nowTick, bool authentication = false);
        void Retire(Peer &peer) noexcept;
        void End(PeerSessionTerminalKind kind, NetworkFailureKind failure, std::uint64_t nowTick) noexcept;
        [[nodiscard]] Result<void> Send(Peer &peer, AdmissionControlKind kind, std::span<const std::byte> payload);
        [[nodiscard]] Result<void> Offer(Peer &peer);
        [[nodiscard]] Result<void> AcceptOffer(Peer &peer, std::span<const std::byte> payload, std::uint64_t nowTick);
        [[nodiscard]] Result<void> AcceptChallenge(Peer &peer, std::span<const std::byte> payload, std::uint64_t nowTick);
        [[nodiscard]] Result<void> AcceptProof(Peer &peer, std::span<const std::byte> payload, std::uint64_t nowTick);
        [[nodiscard]] Result<void> AcceptAcknowledgement(Peer &peer, std::span<const std::byte> payload, std::uint64_t nowTick);
        [[nodiscard]] Result<void> Publish(Peer &peer, const PeerSessionActivation &activation, std::uint64_t nowTick);
        [[nodiscard]] Result<void> Respond(Peer &peer, const AdmissionWire::Acknowledgement &ack);

        INetworkTransport &transport;
        InboundMessageDispatcher &dispatcher;
        std::shared_ptr<const MessageCodecRegistry> codecs;
        NetworkAdmissionPolicy policy;
        NetworkAdmissionProviders providers;
        AdmissionProtection protection;
        std::weak_ptr<INetworkAdmissionObserver> observer;
        std::array<std::optional<Peer>, MaximumProtectedAdmissions> peers;
        std::array<std::uint32_t, MaximumNetworkLifecycleConnections> connectionGenerations{};
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures> supported{};
        std::array<ProtocolFeatureId, MaximumHandshakeFeatures> required{};
        std::vector<MessageDeliveryChannel> gameplayChannels;
        std::uint64_t nextGeneration{1};
        std::uint64_t lastTick{};
        std::size_t framesThisPoll{};
        std::size_t bytesThisPoll{};
        std::thread::id owner{std::this_thread::get_id()};
        bool stopped{};
    };
}  // namespace Horo::Network
