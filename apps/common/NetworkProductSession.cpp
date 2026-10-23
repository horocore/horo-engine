#include "NetworkProductSession.h"

#include "Horo/Network/NetworkErrors.h"

#include <utility>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Preserve exact typed transport identity without forwarding backend text. */
        bool Matches(const ErrorCodeDescriptor *actual, const ErrorCodeDescriptor &expected) noexcept {
            return actual && actual->domain.Value() == expected.domain.Value() && actual->code.Value() == expected.code.Value();
        }
    }  // namespace

    /** @copydoc NetworkProductSessionClock::NetworkProductSessionClock */
    NetworkProductSessionClock::NetworkProductSessionClock(std::shared_ptr<Clock> source, const Duration tickDuration, const Duration epoch)
        : source_(std::move(source)), tickDuration_(tickDuration), epoch_(epoch) {}

    /** @copydoc NetworkProductSessionClock::Validate */
    Result<void> NetworkProductSessionClock::Validate() const {
        if (std::this_thread::get_id() != owner_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkIoWrongThread));
        if (!source_ || tickDuration_.ToNanoseconds() <= 0 || epoch_.ToNanoseconds() < 0)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleInvalid, "Invalid product session clock."));
        return Result<void>::Success();
    }

    /** @copydoc NetworkProductSessionClock::Sample */
    Result<std::uint64_t> NetworkProductSessionClock::Sample() {
        if (std::this_thread::get_id() != owner_)
            return Result<std::uint64_t>::Failure(MakeError(Network::NetworkErrors::NetworkIoWrongThread));
        if (failure_)
            return Result<std::uint64_t>::Failure(*failure_);
        if (const auto valid = Validate(); valid.HasError())
            return Result<std::uint64_t>::Failure(valid.ErrorValue());
        const Duration sample = source_->MonotonicNow();
        if (sample < epoch_ || (lastSample_ && sample < *lastSample_)) {
            failure_ = MakeError(Network::NetworkErrors::NetworkLifecycleInvalid, "Product session clock moved backwards.");
            pollTick_ = 0;
            return Result<std::uint64_t>::Failure(*failure_);
        }
        lastSample_ = sample;
        return Result<std::uint64_t>::Success(
            static_cast<std::uint64_t>((sample - epoch_).ToNanoseconds() / tickDuration_.ToNanoseconds()) + 1);
    }

    /** @copydoc NetworkProductSessionClock::BeginPoll */
    Result<std::uint64_t> NetworkProductSessionClock::BeginPoll() {
        auto sampled = Sample();
        if (sampled.HasValue())
            pollTick_ = sampled.Value();
        return sampled;
    }

    /** @copydoc NetworkProductSessionClock::PollTick */
    Result<std::uint64_t> NetworkProductSessionClock::PollTick() const {
        if (std::this_thread::get_id() != owner_)
            return Result<std::uint64_t>::Failure(MakeError(Network::NetworkErrors::NetworkIoWrongThread));
        if (failure_)
            return Result<std::uint64_t>::Failure(*failure_);
        if (pollTick_ == 0)
            return Result<std::uint64_t>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleInvalid));
        return Result<std::uint64_t>::Success(pollTick_);
    }

    /** @copydoc ClientSessionOwner::ClientSessionOwner */
    ClientSessionOwner::ClientSessionOwner(std::shared_ptr<NetworkProductSessionClock> clock) : clock_(std::move(clock)) {}

    /** @copydoc ClientSessionOwner::Validate */
    Result<void> ClientSessionOwner::Validate() const {
        if (stopped_ || mode_ || transportRetired_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleOperationStale));
        if (!clock_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleInvalid));
        return clock_->Validate();
    }

    /** @copydoc ClientSessionOwner::AttachTransport */
    Result<void> ClientSessionOwner::AttachTransport(Network::INetworkTransport &transport) {
        if (stopped_ || transportRetired_ || (transport_ && transport_ != &transport))
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleOperationStale));
        transport_ = &transport;
        return Result<void>::Success();
    }

    /** @copydoc ClientSessionOwner::DetachTransport */
    void ClientSessionOwner::DetachTransport(const Network::INetworkTransport &transport) noexcept {
        if (transport_ != &transport)
            return;
        TransportEnded();
        transport_ = nullptr;
        transportRetired_ = true;
    }

    /** @copydoc ClientSessionOwner::PollTick */
    Result<std::uint64_t> ClientSessionOwner::PollTick() const {
        if (stopped_ || transportRetired_ || !clock_)
            return Result<std::uint64_t>::Failure(MakeError(Network::NetworkErrors::SessionShuttingDown));
        return clock_->PollTick();
    }

    /** @copydoc ClientSessionOwner::TransportEnded */
    void ClientSessionOwner::TransportEnded(const Network::NetworkFailureKind failure) noexcept {
        if (!peer_)
            return;
        try {
            if (!peer_->Terminal())
                (void)peer_->FailTransport(connection_, generation_, failure, lastTick_);
            if (admitted_)
                (void)Revoke();
        } catch (...) {
            // No provider text or exception crosses the noexcept ingress observer.
            ingressFailed_ = true;
        }
    }

    /** @copydoc ClientSessionOwner::OnTransportEvent */
    void ClientSessionOwner::OnTransportEvent(const Network::NetworkTransportEvent &event) noexcept {
        if (stopped_ || !transport_)
            return;
        using enum Network::NetworkTransportEventKind;
        if (event.kind == Closed || event.kind == Failed) {
            if (peer_ && event.connection == connection_) {
                auto failure = Network::NetworkFailureKind::TransportUnavailable;
                if (Matches(event.failure, Network::NetworkErrors::NameResolutionFailed))
                    failure = Network::NetworkFailureKind::NameResolutionFailed;
                else if (Matches(event.failure, Network::NetworkErrors::TransportReliableBackpressure))
                    failure = Network::NetworkFailureKind::TransportSaturated;
                TransportEnded(failure);
            }
            return;
        }
        if ((event.kind == Accepted || event.kind == Connected) && (!admitted_ || event.connection != connection_)) {
            try {
                (void)transport_->Close(event.connection);
            } catch (...) {
                ingressFailed_ = true;
            }
        }
    }

    /** @copydoc ClientSessionOwner::OnSessionPrepared */
    Result<void> ClientSessionOwner::OnSessionPrepared(const Network::AdmissionEndpoint endpoint,
                                                       const std::shared_ptr<Network::PeerSessionLifecycle> &lifecycle,
                                                       const Network::NetworkOperationGeneration generation, const std::uint64_t nowTick) {
        if (endpoint == Network::AdmissionEndpoint::Server)
            return Result<void>::Success();
        if (endpoint != Network::AdmissionEndpoint::Client || !lifecycle || !mode_ || !mode_->Active() || stopped_ || transportRetired_ ||
            !clock_ || admitted_ || (peer_ && !peer_->Terminal()) || !generation.IsValid() ||
            (generation_.IsValid() && generation <= generation_) || lifecycle->State() != Network::PeerSessionState::Negotiating)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::AdmissionBindingInvalid));
        const auto tick = clock_->PollTick();
        if (tick.HasError())
            return Result<void>::Failure(tick.ErrorValue());
        if (tick.Value() != nowTick)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::AdmissionBindingInvalid));
        lastTick_ = nowTick;
        peer_ = lifecycle;
        connection_ = lifecycle->Connection();
        generation_ = generation;
        return Result<void>::Success();
    }

    /** @copydoc ClientSessionOwner::OnSessionActivated */
    Result<void> ClientSessionOwner::OnSessionActivated(const Network::AdmissionEndpoint endpoint,
                                                        const std::shared_ptr<Network::PeerSessionLifecycle> &lifecycle,
                                                        const Network::NetworkOperationGeneration generation, const std::uint64_t nowTick) {
        if (endpoint == Network::AdmissionEndpoint::Server)
            return Result<void>::Success();
        if (endpoint != Network::AdmissionEndpoint::Client || !lifecycle)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::AdmissionBindingInvalid));
        return Admit(Network::NetworkModeWorldKind::Client, lifecycle, lifecycle->Connection(), generation, nowTick);
    }

    /** @copydoc ClientSessionOwner::Bind */
    void ClientSessionOwner::Bind(Network::NetworkModeComposition &mode) noexcept {
        mode_ = &mode;
    }

    /** @copydoc ClientSessionOwner::Admit */
    Result<void> ClientSessionOwner::Admit(const Network::NetworkModeWorldKind world, std::shared_ptr<Network::PeerSessionLifecycle> peer,
                                           const Network::ConnectionHandle connection, const Network::NetworkOperationGeneration generation,
                                           const std::optional<std::uint64_t> publishedTick) {
        if (stopped_ || transportRetired_ || !mode_ || !mode_->Active())
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeUnavailable));
        if (!peer || !clock_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleInvalid));
        // Native terminal events carry a connection generation, not a session generation.
        // A replacement session must therefore use a distinct transport handle.
        if (peer_ && ((generation == generation_ && (peer != peer_ || connection != connection_)) ||
                      (generation != generation_ && connection == connection_)))
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkLifecycleOperationStale));
        auto now = publishedTick.has_value() ? clock_->PollTick() : Sample(false);
        if (publishedTick.has_value() && now.HasValue() && now.Value() != *publishedTick)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::AdmissionBindingInvalid));
        if (now.HasError())
            return Result<void>::Failure(now.ErrorValue());
        if (auto admitted = mode_->AdmitClientSession(world, *peer, connection, generation, now.Value()); admitted.HasError())
            return admitted;
        if (peer_ && peer_ != peer && !peer_->Terminal())
            (void)peer_->Cancel(connection_, generation_, now.Value());
        peer_ = std::move(peer);
        connection_ = connection;
        generation_ = generation;
        admitted_ = true;
        return Result<void>::Success();
    }

    /** @copydoc ClientSessionOwner::BeginPoll */
    Result<void> ClientSessionOwner::BeginPoll() {
        if (transportRetired_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::SessionShuttingDown));
        if (!clock_)
            return Result<void>::Success();
        auto now = Sample(true);
        if (now.HasError())
            return Result<void>::Failure(now.ErrorValue());
        if (admitted_)
            (void)peer_->Expire(now.Value());
        return Reconcile();
    }

    /** @copydoc ClientSessionOwner::Reconcile */
    Result<void> ClientSessionOwner::Reconcile() {
        if (clock_ && std::this_thread::get_id() != clock_->owner_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkIoWrongThread));
        if (clock_ && clock_->failure_)
            return Result<void>::Failure(*clock_->failure_);
        if (ingressFailed_) {
            if (admitted_)
                (void)Revoke();
            return Result<void>::Failure(MakeError(Network::NetworkErrors::TransportNativeUnavailable));
        }
        if (admitted_ && peer_->State() != Network::PeerSessionState::Active)
            return Revoke();
        return Result<void>::Success();
    }

    /** @copydoc ClientSessionOwner::Disconnect */
    Result<void> ClientSessionOwner::Disconnect(const Network::NetworkModeWorldKind world,
                                                const Network::NetworkOperationGeneration generation) {
        if (!admitted_ || generation != generation_)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeStale));
        if (world != Network::NetworkModeWorldKind::Client)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::NetworkModeInvalid));
        auto now = Sample(false);
        if (now.HasError())
            return Result<void>::Failure(now.ErrorValue());
        auto revoked = Revoke();
        if (!peer_->Terminal())
            (void)peer_->Cancel(connection_, generation_, now.Value());
        return revoked;
    }

    /** @copydoc ClientSessionOwner::Terminal */
    const Network::PeerSessionTerminalSnapshot *ClientSessionOwner::Terminal() const noexcept {
        return peer_ ? peer_->Terminal() : nullptr;
    }

    /** @copydoc ClientSessionOwner::Shutdown */
    void ClientSessionOwner::Shutdown() noexcept {
        if (stopped_)
            return;
        if (admitted_)
            (void)Revoke();
        // Observers borrowed by the canonical lifecycle still exist before service teardown.
        if (peer_)
            (void)peer_->Shutdown(lastTick_);
        stopped_ = true;
        mode_ = nullptr;
    }

    /** @copydoc ClientSessionOwner::Revoke */
    Result<void> ClientSessionOwner::Revoke() {
        admitted_ = false;
        return mode_->DisconnectClient(Network::NetworkModeWorldKind::Client, generation_);
    }

    /** @copydoc ClientSessionOwner::Sample */
    Result<std::uint64_t> ClientSessionOwner::Sample(const bool poll) {
        auto now = poll ? clock_->BeginPoll() : clock_->Sample();
        if (now.HasValue()) {
            lastTick_ = now.Value();
        } else if (peer_ && std::this_thread::get_id() == clock_->owner_) {
            if (admitted_)
                (void)Revoke();
            if (!peer_->Terminal())
                (void)peer_->Cancel(connection_, generation_, lastTick_);
        }
        return now;
    }

}  // namespace Horo::Application::Internal
