#include "NetworkAdmissionState.h"
#include "NetworkProviderBoundary.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Frame scratch owns no persistent proof and clears every byte after a synchronous send. */
        struct FrameScratch final {
            FrameScratch() = default;
            FrameScratch(const FrameScratch &) = delete;
            FrameScratch &operator=(const FrameScratch &) = delete;
            FrameScratch(FrameScratch &&) = delete;
            FrameScratch &operator=(FrameScratch &&) = delete;
            std::array<std::byte, AdmissionWire::MaximumFrameBytes> bytes{};

            ~FrameScratch() {
                Security::SecureZero(bytes);
            }
        };

        /** @brief Convert validated relative limits without overflowing the shared absolute tick domain. */
        std::optional<PeerSessionDeadlines> Deadlines(const NetworkAdmissionTimeouts &timeouts, const std::uint64_t nowTick) {
            if (timeouts.lifetimeTicks > std::numeric_limits<std::uint64_t>::max() - nowTick)
                return std::nullopt;
            return PeerSessionDeadlines{nowTick + timeouts.negotiationTicks, nowTick + timeouts.authenticationTicks,
                                        nowTick + timeouts.activationTicks, nowTick + timeouts.lifetimeTicks, timeouts.inactivityTicks};
        }
    }  // namespace

    Result<void> NetworkAdmissionController::Impl::Connect(const NetworkTransportEvent &event, const std::uint64_t nowTick) {
        const auto endpoint = event.kind == NetworkTransportEventKind::Connected ? AdmissionEndpoint::Client : AdmissionEndpoint::Server;
        if (!event.connection.IsValid() || event.connection.Slot() >= connectionGenerations.size() ||
            !policy.endpoints[static_cast<std::size_t>(endpoint)]) {
            (void)transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        }
        auto &latest = connectionGenerations[event.connection.Slot()];
        if (event.connection.Generation() <= latest || nextGeneration == std::numeric_limits<std::uint64_t>::max()) {
            if (auto *existing = Find(event.connection))
                return Fail(*existing, NetworkFailureKind::ProtocolMalformed, nowTick);
            (void)transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionReplayRejected));
        }
        latest = event.connection.Generation();
        const auto slots = std::span{peers}.first(policy.maximumPeers);
        auto slot = std::ranges::find_if(slots, [](const auto &entry) {
            return !entry || entry->lifecycle->Terminal();
        });
        const auto deadlines = Deadlines(policy.timeouts, nowTick);
        if (slot == slots.end() || !deadlines) {
            (void)transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        }
        if (auto initialized = InitializePeer(*slot, event, endpoint, *deadlines); initialized.HasError())
            return initialized;
        auto &peer = **slot;
        const auto generated = Detail::InvokeAdmissionProvider([this, &peer] {
            return providers.random->Fill(peer.localNonce);
        });
        if (!generated || generated->HasError() || std::ranges::all_of(peer.localNonce, [](const auto byte) {
            return byte == std::byte{};
        }))
            return Fail(peer, NetworkFailureKind::SessionAuthenticationRejected, nowTick, true);
        if (peer.lifecycle->BeginNegotiation(peer.connection, peer.generation, nowTick).HasError())
            return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        if (auto projected = ProjectPrepared(peer, nowTick); projected.HasError())
            return projected;
        if (auto sent = Offer(peer); sent.HasError())
            return Fail(peer, NetworkFailureKind::TransportUnavailable, nowTick);
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::InitializePeer(std::optional<Peer> &slot, const NetworkTransportEvent &event,
                                                                  const AdmissionEndpoint endpoint, const PeerSessionDeadlines &deadlines) {
        const auto generation = NetworkOperationGeneration::Create(nextGeneration++).Value();
        auto created = PeerSessionLifecycle::Create(event.connection, generation, deadlines);
        auto negotiation = HandshakeNegotiator::Create(event.connection, generation, deadlines.negotiationTick, policy.local);
        if (created.HasError() || negotiation.HasError()) {
            (void)transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        }
        if (slot)
            Retire(*slot);
        try {
            slot.emplace(Peer{.connection = event.connection,
                              .generation = generation,
                              .endpoint = endpoint,
                              .deadlines = deadlines,
                              .lifecycle = std::make_shared<PeerSessionLifecycle>(std::move(created).Value()),
                              .negotiation = std::move(negotiation).Value()});
        } catch (const std::bad_alloc &) {
            (void)transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        }
        return Result<void>::Success();
    }

    void NetworkAdmissionController::Impl::Retire(Peer &peer) noexcept {
        if (peer.retirement != Peer::Retirement::Live)
            return;
        peer.retirement = Peer::Retirement::Closing;
        dispatcher.RevokeSession(peer.connection);
        if (peer.negotiation)
            (void)peer.negotiation->Shutdown();
        if (peer.authentication)
            (void)peer.authentication->Shutdown();
        if (peer.ledgerReserved) {
            (void)protection.End(peer.connection, peer.generation);
            peer.ledgerReserved = false;
        }
        const auto closed = Detail::InvokeAdmissionProvider([this, &peer] {
            return transport.Close(peer.connection);
        });
        // Cleanup status is allocation-free and never replaces the original canonical terminal reason.
        if (!closed)
            peer.retirement = Peer::Retirement::CallbackFailed;
        else if (closed->HasError())
            peer.retirement = Peer::Retirement::Rejected;
        else
            peer.retirement = Peer::Retirement::Closed;
    }

    /** @brief Local owner retirement cancels without blaming peer authentication. */
    Result<void> NetworkAdmissionController::Impl::AbortProjection(Peer &peer, const std::uint64_t nowTick) {
        if (!peer.lifecycle->Terminal())
            (void)peer.lifecycle->Cancel(peer.connection, peer.generation, nowTick);
        Retire(peer);
        return Result<void>::Failure(MakeError(NetworkErrors::SessionCancelled));
    }

    /** @brief Required client observation must survive to the actual pending publication boundary. */
    Result<void> NetworkAdmissionController::Impl::ProjectPrepared(Peer &peer, const std::uint64_t nowTick) {
        auto projection = observer.lock();
        if (!projection && peer.endpoint == AdmissionEndpoint::Client)
            return AbortProjection(peer, nowTick);
        const auto projected = Detail::InvokeAdmissionProvider([&peer, &projection, nowTick] {
            return !projection || projection->OnSessionPrepared(peer.endpoint, peer.lifecycle, peer.generation, nowTick).HasValue();
        });
        if (!projected.has_value() || !*projected)
            return AbortProjection(peer, nowTick);
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::Fail(Peer &peer, const NetworkFailureKind failure, const std::uint64_t nowTick,
                                                        const bool authentication) {
        if (!peer.lifecycle->Terminal()) {
            if (peer.lifecycle->State() == PeerSessionState::Active)
                (void)peer.lifecycle->Cancel(peer.connection, peer.generation, nowTick);
            else if (authentication)
                (void)peer.lifecycle->RejectAuthentication(peer.connection, peer.generation, nowTick);
            else if (failure == NetworkFailureKind::ProtocolMalformed || failure == NetworkFailureKind::ProtocolIncompatible)
                (void)peer.lifecycle->RejectProtocol(peer.connection, peer.generation, failure, nowTick);
            else
                (void)peer.lifecycle->FailTransport(peer.connection, peer.generation, failure, nowTick);
        }
        Retire(peer);
        if (const auto *terminal = peer.lifecycle->Terminal(); terminal && terminal->failure)
            return Result<void>::Failure(terminal->failure->ToError());
        return Result<void>::Failure(MakeError(NetworkErrors::SessionRemoteRejected));
    }

    Result<void> NetworkAdmissionController::Impl::Send(Peer &peer, const AdmissionControlKind kind,
                                                        const std::span<const std::byte> payload) {
        if (peer.nextSequence == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(NetworkErrors::MessageCounterExhausted));
        const auto &identity = policy.controls[static_cast<std::size_t>(kind)];
        const MessageEnvelopePayloadView message{identity.protocol,
                                                 identity.id,
                                                 identity.schema,
                                                 identity.version,
                                                 MessageSequenceNumber{peer.nextSequence},
                                                 {},
                                                 0,
                                                 payload};
        FrameScratch frame;
        const auto written = EncodeMessageEnvelopePayloadInto(message, *codecs, frame.bytes, policy.envelope);
        if (written.HasError())
            return Result<void>::Failure(written.ErrorValue());
        auto sent =
            transport.Send(peer.connection, ChannelId{}, std::span{frame.bytes}.first(written.Value()), DeliveryPolicy::ReliableOrdered);
        if (sent.HasValue())
            ++peer.nextSequence;
        return sent;
    }

    Result<void> NetworkAdmissionController::Impl::Offer(Peer &peer) {
        FrameScratch payload;
        AdmissionWire::Writer writer{payload.bytes};
        if (!AdmissionWire::WriteOffer(writer, policy, peer.localNonce))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        return Send(peer, AdmissionControlKind::Offer, writer.Written());
    }

    Result<void> NetworkAdmissionController::Impl::Respond(Peer &peer, const AdmissionWire::Acknowledgement &ack) {
        FrameScratch payload;
        AdmissionWire::Writer writer{payload.bytes};
        if (!AdmissionWire::WriteAcknowledgement(writer, ack))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        return Send(peer, AdmissionControlKind::Acknowledgement, writer.Written());
    }
}  // namespace Horo::Network
