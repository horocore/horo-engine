#include "NetworkAdmissionState.h"

#include <algorithm>

namespace Horo::Network {
    namespace {
        /** @brief Nonces/digests must have a real nonzero representation; this is not an authenticity check. */
        bool NonZero(const std::span<const std::byte> value) {
            return std::ranges::any_of(value, [](const auto byte) {
                return byte != std::byte{};
            });
        }

        /** @brief Shared client/server nonce order is independent of endpoint-local connection handles. */
        bool SameNonces(const AuthenticationChallenge &left, const AuthenticationChallenge &right) noexcept {
            return left.clientNonce == right.clientNonce && left.serverNonce == right.serverNonce;
        }

        /** @brief Compare peer-public selection fields while retaining local generation fences separately. */
        bool SameNegotiation(const HandshakeSelection &left, const HandshakeSelection &right) noexcept {
            return left.protocol == right.protocol && left.version == right.version && left.schemaFingerprint == right.schemaFingerprint &&
                   left.features == right.features && left.compression == right.compression;
        }

        struct Scratch final {
            std::array<std::byte, AdmissionWire::MaximumFrameBytes> bytes{};

            ~Scratch() {
                Security::SecureZero(bytes);
            }
        };
    }  // namespace

    Result<void> NetworkAdmissionController::Impl::AcceptOffer(Peer &peer, const std::span<const std::byte> payload,
                                                               const std::uint64_t nowTick) {
        if (peer.lifecycle->State() != PeerSessionState::Negotiating)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        AdmissionWire::Offer offer;
        if (!AdmissionWire::ReadOffer(payload, offer) || !NonZero(offer.nonce) || offer.nonce == peer.localNonce)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        auto selected = peer.negotiation->Accept(peer.connection, peer.generation, offer.View(), nowTick);
        // The decoded, validated peer descriptor owns this revision; local handles never select it.
        auto peerTransport = ResolveTransportCapabilities(offer.capabilities, offer.capabilities.revision, policy.requirements);
        if (selected.HasError() || peerTransport.HasError())
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        if (peer.lifecycle->AcceptNegotiation(selected.Value(), nowTick).HasError())
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        peer.remoteNonce = offer.nonce;
        peer.expectedPeerTransport = peerTransport.Value();
        peer.challenge = {.connection = peer.connection,
                          .sessionGeneration = peer.generation,
                          .policy = policy.trust.id,
                          .policyRevision = policy.trust.revision,
                          .clientNonce = peer.endpoint == AdmissionEndpoint::Client ? peer.localNonce : peer.remoteNonce,
                          .serverNonce = peer.endpoint == AdmissionEndpoint::Server ? peer.localNonce : peer.remoteNonce};
        peer.challenge.transcriptDigest = ComputeAdmissionTranscriptDigest(selected.Value(), peer.challenge);
        if (protection.Begin(policy.sourceBucket, selected.Value(), peer.challenge, nowTick).HasError())
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        peer.ledgerReserved = true;
        const AuthenticationAuthorities authorities{providers.certificates.get(), providers.peers.get(), providers.credentials.get(),
                                                    providers.privateKeys.get()};
        auto authentication =
            AuthenticationSessionAdapter::Create(policy.trust, peer.challenge, authorities, peer.deadlines.authenticationTick, &protection);
        if (authentication.HasError())
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        peer.authentication.emplace(std::move(authentication).Value());
        Scratch bytes;
        AdmissionWire::Writer writer{bytes.bytes};
        if (!AdmissionWire::WriteChallenge(writer, {selected.Value(), peer.challenge}))
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        if (Send(peer, AdmissionControlKind::Challenge, writer.Written()).HasError())
            return Fail(peer, NetworkFailureKind::TransportUnavailable, nowTick);
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::AcceptChallenge(Peer &peer, const std::span<const std::byte> payload,
                                                                   const std::uint64_t nowTick) {
        if (peer.lifecycle->State() != PeerSessionState::Authenticating || !peer.authentication || peer.peerChallenge ||
            !peer.expectedPeerTransport)
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        AdmissionWire::Challenge challenge;
        if (!AdmissionWire::ReadChallenge(payload, challenge) || !SameNonces(challenge.challenge, peer.challenge) ||
            challenge.challenge.policy != policy.trust.id || challenge.challenge.policyRevision != policy.trust.revision ||
            !NonZero(challenge.challenge.transcriptDigest) || !SameNegotiation(challenge.selection, *peer.lifecycle->Negotiation()) ||
            challenge.selection.transport != *peer.expectedPeerTransport)
            return Fail(peer, NetworkFailureKind::ProtocolIncompatible, nowTick);
        // Only local context reaches providers; the opaque peer-owned transcript digest is preserved verbatim.
        challenge.challenge.connection = peer.connection;
        challenge.challenge.sessionGeneration = peer.generation;
        challenge.selection.connection = peer.connection;
        challenge.selection.sessionGeneration = peer.generation;
        peer.peerChallenge = challenge;
        try {
            auto evidence = providers.channel->Inspect(peer.connection, peer.generation, nowTick);
            if (evidence.HasError())
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            // Canonical peer authorization is performed here before issuing any outbound proof.
            auto authorized = peer.authentication->AuthorizeProofIssuance(peer.connection, peer.generation, challenge.challenge,
                                                                          evidence.Value(), nowTick);
            if (authorized.HasError())
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            peer.issuance = authorized.Value();
            auto proof = providers.proofs->Issue({authorized.Value(), *peer.lifecycle->Negotiation(), nowTick});
            if (proof.HasError() || proof.Value().Empty() || proof.Value().View().size() > policy.trust.maximumProofBytes)
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            auto refreshed = providers.channel->Inspect(peer.connection, peer.generation, nowTick);
            if (refreshed.HasError() || !authorized.Value().MatchesEvidence(refreshed.Value(), nowTick))
                return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
            Scratch bytes;
            AdmissionWire::Writer writer{bytes.bytes};
            const AdmissionWire::Proof response{{.policy = challenge.challenge.policy,
                                                 .policyRevision = challenge.challenge.policyRevision,
                                                 .transcriptDigest = challenge.challenge.transcriptDigest,
                                                 .proof = proof.Value().View()},
                                                challenge.challenge.clientNonce,
                                                challenge.challenge.serverNonce};
            if (!AdmissionWire::WriteProof(writer, response) || Send(peer, AdmissionControlKind::Proof, writer.Written()).HasError())
                return Fail(peer, NetworkFailureKind::TransportUnavailable, nowTick);
            peer.proofIssued = true;
            return Result<void>::Success();
        } catch (...) {
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        }
    }
}  // namespace Horo::Network
