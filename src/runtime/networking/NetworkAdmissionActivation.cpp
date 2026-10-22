#include "NetworkAdmissionState.h"

#include <algorithm>

namespace Horo::Network {
    Result<void> NetworkAdmissionController::Impl::AcceptProof(Peer &peer, const std::span<const std::byte> payload,
                                                               const std::uint64_t nowTick) {
        if (peer.lifecycle->State() != PeerSessionState::Authenticating || !peer.authentication || !peer.peerChallenge || !peer.proofIssued)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        AdmissionWire::Proof proof;
        if (!AdmissionWire::ReadProof(payload, proof, policy.trust.maximumProofBytes) || proof.clientNonce != peer.challenge.clientNonce ||
            proof.serverNonce != peer.challenge.serverNonce)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        try {
            auto evidence = providers.channel->Inspect(peer.connection, peer.generation, nowTick);
            if (evidence.HasError())
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            const auto authenticated =
                peer.authentication->Authenticate(peer.connection, peer.generation, proof.response, evidence.Value(), nowTick);
            if (authenticated.HasError() || peer.lifecycle->AcceptAuthentication(authenticated.Value(), nowTick).HasError())
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            const AdmissionWire::Acknowledgement proposal{false, peer.challenge.transcriptDigest,
                                                          authenticated.Value().secureChannel.bindingDigest, peer.challenge.clientNonce,
                                                          peer.challenge.serverNonce};
            if (Respond(peer, proposal).HasError())
                return Fail(peer, NetworkFailureKind::TransportUnavailable, nowTick);
            return Result<void>::Success();
        } catch (...) {
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        }
    }

    Result<void> NetworkAdmissionController::Impl::Publish(Peer &peer, const PeerSessionActivation &activation,
                                                           const std::uint64_t nowTick) {
        auto projection = observer.lock();
        if (!projection && peer.endpoint == AdmissionEndpoint::Client)
            return AbortProjection(peer, nowTick);
        if (auto activated = peer.lifecycle->Activate(activation, nowTick); activated.HasError())
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        const auto *selection = peer.lifecycle->Negotiation();
        auto gate = MessageDeliveryGate::Create(selection->transport, selection->transport.capabilityRevision, peer.connection,
                                                peer.generation, gameplayChannels);
        if (gate.HasError())
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        if (dispatcher.RegisterSession(peer.lifecycle, peer.connection, peer.generation, std::move(gate).Value(), nowTick).HasError())
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        try {
            if (projection && projection->OnSessionActivated(peer.endpoint, peer.lifecycle, peer.generation, nowTick).HasError())
                return AbortProjection(peer, nowTick);
        } catch (...) {
            return AbortProjection(peer, nowTick);
        }
        (void)protection.End(peer.connection, peer.generation);
        peer.ledgerReserved = false;
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::AcceptAcknowledgement(Peer &peer, const std::span<const std::byte> payload,
                                                                         const std::uint64_t nowTick) {
        if (peer.lifecycle->State() != PeerSessionState::Activating || !peer.peerChallenge || !peer.authentication || !peer.proofIssued)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        AdmissionWire::Acknowledgement ack;
        const auto *authentication = peer.lifecycle->Authentication();
        if (!AdmissionWire::ReadAcknowledgement(payload, ack) || ack.clientNonce != peer.challenge.clientNonce ||
            ack.serverNonce != peer.challenge.serverNonce || !authentication || std::ranges::all_of(ack.binding, [](const auto byte) {
            return byte == std::byte{};
        }))
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        if (!ack.response) {
            if (peer.proposalReceived || ack.transcript != peer.peerChallenge->challenge.transcriptDigest)
                return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
            peer.proposalReceived = true;
            ack.response = true;
            if (Respond(peer, ack).HasError())
                return Fail(peer, NetworkFailureKind::TransportUnavailable, nowTick);
            return Result<void>::Success();
        }
        if (!peer.proposalReceived || peer.proposalAcknowledged || ack.transcript != peer.challenge.transcriptDigest ||
            ack.binding != authentication->secureChannel.bindingDigest)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        try {
            const auto evidence = providers.channel->Inspect(peer.connection, peer.generation, nowTick);
            const auto &channel = authentication->secureChannel;
            if (evidence.HasError() || !peer.issuance || !peer.issuance->MatchesEvidence(evidence.Value(), nowTick) ||
                peer.authentication->ValidateActivationEvidence(peer.connection, peer.generation, evidence.Value(), nowTick).HasError())
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            peer.proposalAcknowledged = true;
            return Publish(peer, {peer.connection, peer.generation, channel.channel, channel.channelGeneration, ack.binding}, nowTick);
        } catch (...) {
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        }
    }
}  // namespace Horo::Network
