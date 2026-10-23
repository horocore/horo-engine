#include "NetworkAdmissionState.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Reject malformed host deadlines before any transport or credential work. */
        bool OrderedTimeouts(const NetworkAdmissionTimeouts &timeouts) noexcept {
            return timeouts.negotiationTicks != 0 && timeouts.negotiationTicks < timeouts.authenticationTicks &&
                   timeouts.authenticationTicks < timeouts.activationTicks && timeouts.activationTicks < timeouts.lifetimeTicks &&
                   timeouts.inactivityTicks != 0;
        }
    }  // namespace

    NetworkAdmissionController::Impl::Impl(INetworkTransport &transportInput, InboundMessageDispatcher &dispatcherInput,
                                           std::shared_ptr<const MessageCodecRegistry> codecsInput,
                                           const NetworkAdmissionPolicy &policyInput, NetworkAdmissionProviders providersInput,
                                           AdmissionProtection protectionInput, std::weak_ptr<INetworkAdmissionObserver> observerInput)
        : transport(transportInput), dispatcher(dispatcherInput), codecs(std::move(codecsInput)), policy(policyInput),
          providers(std::move(providersInput)), protection(std::move(protectionInput)), observer(std::move(observerInput)) {}

    Result<void> NetworkAdmissionController::Impl::ValidatePolicy() {
        const auto &features = policy.local.compatibility.features;
        if (!codecs || !dispatcher.UsesCodecRegistry(*codecs) || !providers.certificates || !providers.peers || !providers.credentials ||
            !providers.privateKeys || !providers.random || !providers.proofs || !providers.channel)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        if (auto bounds = ValidateBounds(); bounds.HasError())
            return bounds;
        if (features.supported.size() > supported.size() || features.required.size() > required.size() || policy.gameplayChannels.empty() ||
            policy.gameplayChannels.size() > 64)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        if (auto controls = ValidateControls(); controls.HasError())
            return controls;
        std::ranges::copy(features.supported, supported.begin());
        std::ranges::copy(features.required, required.begin());
        policy.local.compatibility.features = {std::span{supported}.first(features.supported.size()),
                                               std::span{required}.first(features.required.size())};
        std::ranges::sort(std::span{supported}.first(features.supported.size()));
        std::ranges::sort(std::span{required}.first(features.required.size()));
        gameplayChannels.assign(policy.gameplayChannels.begin(), policy.gameplayChannels.end());
        policy.gameplayChannels = gameplayChannels;
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::ValidateBounds() const {
        if (transport.Capabilities() != policy.local.transport || ValidateHandshakeLocalDescriptor(policy.local).HasError() ||
            !ValidateTransportCapabilities(policy.local.transport) ||
            ResolveTransportCapabilities(policy.local.transport, policy.local.transport.revision, policy.requirements).HasError() ||
            !policy.requirements.requiredDelivery[static_cast<std::size_t>(DeliveryPolicy::ReliableOrdered)] ||
            (policy.endpoints[static_cast<std::size_t>(AdmissionEndpoint::Client)] && observer.expired()) ||
            policy.envelope.maximumFrameBytes > policy.local.transport.maximumMessageBytes)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        if (policy.sourceBucket.value == 0 || policy.maximumPeers == 0 || policy.maximumPeers > peers.size() ||
            policy.maximumControlFramesPerPoll == 0 || policy.maximumControlFramesPerPoll > MaximumNetworkLifecycleConnections ||
            policy.maximumControlBytesPerPoll == 0 || policy.envelope.maximumFrameBytes > AdmissionWire::MaximumFrameBytes ||
            policy.envelope.maximumPayloadBytes < 115 || policy.trust.maximumProofBytes > policy.envelope.maximumPayloadBytes - 115 ||
            !ValidateNetworkTrustPolicy(policy.trust) || !OrderedTimeouts(policy.timeouts) ||
            !std::ranges::any_of(policy.endpoints, [](const bool enabled) {
            return enabled;
        }))
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        return Result<void>::Success();
    }

    Result<void> NetworkAdmissionController::Impl::ValidateControls() const {
        for (std::size_t i = 0; i < policy.controls.size(); ++i) {
            const auto &control = policy.controls[i];
            if (const auto codec = codecs->FindCodec(control.protocol, control.id);
                control.optional || control.protocol != policy.local.compatibility.protocol || !codec || codec->schema != control.schema ||
                !codec->supportedVersions.Contains(control.version) || control.version != MessageSchemaVersion{1, 0})
                return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
            for (std::size_t j = 0; j < i; ++j)
                if (policy.controls[j].id == control.id)
                    return Result<void>::Failure(MakeError(NetworkErrors::AdmissionPolicyInvalid));
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkAdmissionController::Create */
    Result<std::shared_ptr<NetworkAdmissionController>> NetworkAdmissionController::Create(
        INetworkTransport &transport, InboundMessageDispatcher &dispatcher, std::shared_ptr<const MessageCodecRegistry> codecs,
        const NetworkAdmissionPolicy &policy, NetworkAdmissionProviders providers, std::weak_ptr<INetworkAdmissionObserver> observer) {
        auto protection = AdmissionProtection::Create(policy.protection);
        if (protection.HasError())
            return Result<std::shared_ptr<NetworkAdmissionController>>::Failure(protection.ErrorValue());
        try {
            auto impl = std::make_unique<Impl>(transport, dispatcher, std::move(codecs), policy, std::move(providers),
                                               std::move(protection).Value(), std::move(observer));
            if (auto valid = impl->ValidatePolicy(); valid.HasError())
                return Result<std::shared_ptr<NetworkAdmissionController>>::Failure(valid.ErrorValue());
            return Result<std::shared_ptr<NetworkAdmissionController>>::Success(
                std::make_shared<NetworkAdmissionController>(ConstructionKey{}, std::move(impl)));
        } catch (const std::bad_alloc &) {
            return Result<std::shared_ptr<NetworkAdmissionController>>::Failure(MakeError(NetworkErrors::AdmissionLimitExceeded));
        }
    }

    NetworkAdmissionController::NetworkAdmissionController(ConstructionKey, std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    NetworkAdmissionController::~NetworkAdmissionController() {
        Shutdown(impl_->lastTick);
    }

    Result<void> NetworkAdmissionController::Impl::CheckTick(const std::uint64_t nowTick) const {
        if (owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        if (stopped)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        if (nowTick == 0 || nowTick != lastTick)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        return Result<void>::Success();
    }

    /** @copydoc NetworkAdmissionController::BeginPoll */
    Result<void> NetworkAdmissionController::BeginPoll(const std::uint64_t nowTick) {
        if (impl_->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkIoWrongThread));
        if (nowTick == 0 || nowTick < impl_->lastTick) {
            Cancel(impl_->lastTick);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        }
        if (impl_->stopped)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionShuttingDown));
        impl_->lastTick = nowTick;
        impl_->framesThisPoll = 0;
        impl_->bytesThisPoll = 0;
        for (auto &entry : impl_->peers) {
            if (!entry)
                continue;
            if (entry->lifecycle->Terminal()) {
                impl_->Retire(*entry);
                continue;
            }
            if (entry->lifecycle->Expire(nowTick))
                impl_->Retire(*entry);
        }
        return Result<void>::Success();
    }

    NetworkAdmissionController::Impl::Peer *NetworkAdmissionController::Impl::Find(const ConnectionHandle connection) noexcept {
        for (auto &entry : peers)
            if (entry && entry->connection == connection)
                return &*entry;
        return nullptr;
    }

    /** @copydoc NetworkAdmissionController::OnTransportEvent */
    Result<AdmissionEventDisposition> NetworkAdmissionController::OnTransportEvent(const NetworkTransportEvent &event,
                                                                                   const std::uint64_t nowTick) {
        using enum AdmissionEventDisposition;
        if (auto tick = impl_->CheckTick(nowTick); tick.HasError())
            return Result<AdmissionEventDisposition>::Failure(tick.ErrorValue());
        using enum NetworkTransportEventKind;
        if (event.kind == Accepted || event.kind == Connected) {
            const auto connected = impl_->Connect(event, nowTick);
            if (connected.HasValue())
                return Result<AdmissionEventDisposition>::Success(PendingCreated);
            const auto &error = connected.ErrorValue();
            const bool cancelled = error.domain.Value() == NetworkErrors::SessionCancelled.domain.Value() &&
                                   error.code.Value() == NetworkErrors::SessionCancelled.code.Value();
            return Result<AdmissionEventDisposition>::Success(cancelled ? CandidateCancelled : PeerRejected);
        }
        if (event.kind != Closed && event.kind != Failed)
            return Result<AdmissionEventDisposition>::Success(Ignored);
        auto *peer = impl_->Find(event.connection);
        if (!peer)
            return Result<AdmissionEventDisposition>::Success(Ignored);
        if (!peer->lifecycle->Terminal()) {
            if (peer->lifecycle->State() == PeerSessionState::Closing)
                (void)peer->lifecycle->CompleteClose(peer->connection, peer->generation, nowTick);
            else
                (void)peer->lifecycle->FailTransport(peer->connection, peer->generation, NetworkFailureKind::TransportUnavailable, nowTick);
        }
        impl_->Retire(*peer);
        return Result<AdmissionEventDisposition>::Success(Terminalized);
    }

    /** @copydoc NetworkAdmissionController::ConsumeControl */
    Result<void> NetworkAdmissionController::ConsumeControl(const NetworkTransportEvent &event, const std::uint64_t nowTick) {
        if (auto tick = impl_->CheckTick(nowTick); tick.HasError())
            return tick;
        auto *peer = impl_->Find(event.connection);
        if (!peer || peer->lifecycle->Terminal()) {
            (void)impl_->transport.Close(event.connection);
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        }
        if (impl_->framesThisPoll >= impl_->policy.maximumControlFramesPerPoll ||
            event.payload.size() > impl_->policy.maximumControlBytesPerPoll - impl_->bytesThisPoll)
            return impl_->Fail(*peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        ++impl_->framesThisPoll;
        impl_->bytesThisPoll += event.payload.size();
        if (event.channel.Value() != 0 || event.delivery != DeliveryPolicy::ReliableOrdered)
            return impl_->Fail(*peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        if (peer->ledgerReserved) {
            auto charged =
                impl_->protection.Charge(peer->connection, peer->generation, AdmissionWork::ParsedBytes, event.payload.size(), nowTick);
            if (charged.HasError())
                return impl_->Fail(*peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        }
        const auto frame = InspectMessageEnvelope(event.payload, *impl_->codecs, impl_->policy.envelope);
        if (frame.HasError() || frame.Value().fieldCount != 0 || frame.Value().sequence.Value() == 0 ||
            frame.Value().sequence.Value() <= peer->lastSequence)
            return impl_->Fail(*peer, NetworkFailureKind::ProtocolMalformed, nowTick);
        peer->lastSequence = frame.Value().sequence.Value();
        return impl_->DispatchControl(*peer, frame.Value(), nowTick);
    }

    Result<void> NetworkAdmissionController::Impl::DispatchControl(Peer &peer, const MessageEnvelopePayloadView &frame,
                                                                   const std::uint64_t nowTick) {
        for (std::size_t i = 0; i < policy.controls.size(); ++i) {
            if (const auto &control = policy.controls[i]; frame.protocol != control.protocol || frame.message != control.id ||
                                                          frame.schema != control.schema || frame.schemaVersion != control.version)
                continue;
            switch (static_cast<AdmissionControlKind>(i)) {
                case AdmissionControlKind::Offer:
                    return AcceptOffer(peer, frame.payload, nowTick);
                case AdmissionControlKind::Challenge:
                    return AcceptChallenge(peer, frame.payload, nowTick);
                case AdmissionControlKind::Proof:
                    return AcceptProof(peer, frame.payload, nowTick);
                case AdmissionControlKind::Acknowledgement:
                    return AcceptAcknowledgement(peer, frame.payload, nowTick);
                case AdmissionControlKind::Count:
                    break;
            }
        }
        return Fail(peer, NetworkFailureKind::ProtocolMalformed, nowTick);
    }

    /** @copydoc NetworkAdmissionController::Cancel */
    void NetworkAdmissionController::Cancel(const std::uint64_t nowTick) noexcept {
        impl_->End(PeerSessionTerminalKind::LocalCancellation, NetworkFailureKind::SessionCancelled, nowTick);
    }

    /** @copydoc NetworkAdmissionController::FailTransport */
    void NetworkAdmissionController::FailTransport(const NetworkFailureKind failure, const std::uint64_t nowTick) noexcept {
        impl_->End(PeerSessionTerminalKind::TransportFailed, failure, nowTick);
    }

    /** @copydoc NetworkAdmissionController::Shutdown */
    void NetworkAdmissionController::Shutdown(const std::uint64_t nowTick) noexcept {
        impl_->End(PeerSessionTerminalKind::Shutdown, NetworkFailureKind::SessionShutdown, nowTick);
    }

    void NetworkAdmissionController::Impl::End(const PeerSessionTerminalKind kind, const NetworkFailureKind failure,
                                               const std::uint64_t nowTick) noexcept {
        if (stopped)
            return;
        stopped = true;
        const auto terminalTick = std::max(nowTick, lastTick);
        for (auto &peer : peers) {
            if (!peer)
                continue;
            if (kind == PeerSessionTerminalKind::LocalCancellation)
                (void)peer->lifecycle->Cancel(peer->connection, peer->generation, terminalTick);
            else if (kind == PeerSessionTerminalKind::TransportFailed)
                (void)peer->lifecycle->FailTransport(peer->connection, peer->generation, failure, terminalTick);
            else
                (void)peer->lifecycle->Shutdown(terminalTick);
            Retire(*peer);
        }
        protection.Shutdown();
        observer.reset();
    }
}  // namespace Horo::Network
