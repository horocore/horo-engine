#include "NetworkProviderBoundary.h"
#include "NetworkWorldBindingState.h"

#include <algorithm>
#include <limits>

namespace Horo::Network {
    Result<void> NetworkWorldBindingController::Impl::ValidatePeer(const PeerSessionLifecycle &peer,
                                                                   const NetworkOperationGeneration generation,
                                                                   const std::uint64_t tick) const {
        if (auto active = peer.AdmitGameplay(peer.Connection(), generation, tick); active.HasError())
            return active;
        if (const auto *selection = peer.Negotiation();
            !selection || selection->protocol != setup.policy.control.protocol || selection->version != version ||
            selection->schemaFingerprint != setup.schemaFingerprint || !setup.controlChannel.IsWithin(selection->transport.channelCount) ||
            std::ranges::find(selection->features.Values(), setup.policy.feature) == selection->features.Values().end())
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        if (setup.policy.authoredScene) {
            const auto features = peer.Negotiation()->features.Values();
            if (std::ranges::find(features, setup.policy.authoredScene->feature) == features.end())
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        }
        if (setup.policy.initialState) {
            const auto features = peer.Negotiation()->features.Values();
            if (std::ranges::find(features, setup.policy.initialState->feature) == features.end() ||
                std::ranges::find(features, setup.policy.prefabLifecycle->feature) == features.end())
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::BindPeer */
    Result<void> NetworkWorldBindingController::BindPeer(const AdmissionEndpoint endpoint, std::shared_ptr<PeerSessionLifecycle> peer,
                                                         const NetworkOperationGeneration generation, const std::uint64_t nowTick) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (!peer || nowTick < impl_->nowTick)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (auto validated = impl_->ValidatePeer(*peer, generation, nowTick); validated.HasError())
            return validated;
        const bool server = endpoint == AdmissionEndpoint::Server;
        if ((server && impl_->role != NetworkProjectRole::DedicatedServer && impl_->role != NetworkProjectRole::ListenServer) ||
            (!server && (endpoint != AdmissionEndpoint::Client ||
                         (impl_->role != NetworkProjectRole::Client && impl_->role != NetworkProjectRole::ListenServer))))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        const auto end = impl_->peers.begin() + impl_->setup.maximumPeers;
        if (auto existing = std::find_if(impl_->peers.begin(), end,
                                         [&peer](const Impl::Peer &slot) {
            return slot.lifecycle && slot.connection == peer->Connection();
        });
            existing != end)
            return existing->lifecycle == peer && existing->generation == generation && existing->sendsWorld == server
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        auto vacant = std::find_if(impl_->peers.begin(), end, [server](const Impl::Peer &slot) {
            return !slot.lifecycle || (!server && !slot.sendsWorld);
        });
        if (vacant == end)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        if (!server) {
            if (auto bound = impl_->worlds.BindClient(peer, peer->Connection(), generation, nowTick); bound.HasError())
                return bound;
        }
        if (vacant->lifecycle && impl_->dispatcher)
            impl_->dispatcher->RevokeSession(vacant->connection);
        impl_->nowTick = nowTick;
        *vacant = {.lifecycle = peer, .connection = peer->Connection(), .generation = generation, .sendsWorld = server};
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::AdmitWorldDispatch */
    Result<void> NetworkWorldBindingController::AdmitWorldDispatch(const ConnectionHandle connection,
                                                                   const NetworkOperationGeneration generation,
                                                                   const PeerSessionLifecycle &lifecycle, const std::uint64_t tick) const {
        if (impl_->stopped || impl_->detached || std::this_thread::get_id() != impl_->owner)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        const auto peer = std::ranges::find_if(impl_->peers, [&](const Impl::Peer &slot) {
            return slot.lifecycle.get() == &lifecycle && slot.connection == connection && slot.generation == generation;
        });
        if (peer == impl_->peers.end())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (auto admitted = impl_->ValidatePeer(lifecycle, generation, tick); admitted.HasError())
            return admitted;
        if (!impl_->InitialConfirmed(*peer))
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationInitialStatePending));
        if (peer->sendsWorld && !impl_->AuthoredSent(*peer, peer->establishedWorld))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (!impl_->worlds.ReadyForPeer(peer->sendsWorld))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        return Result<void>::Success();
    }

    Result<void> NetworkWorldBindingController::Impl::Queue(Peer &peer, const NetworkWorldBindingRecord &record,
                                                            const std::uint64_t tick) const {
        if (peer.frameBytes != 0)
            return peer.pending && *peer.pending == record ? Result<void>::Success()
                                                           : Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
        const auto payload = EncodeNetworkWorldBinding(record);
        if (payload.HasError())
            return Result<void>::Failure(payload.ErrorValue());
        const auto &control = setup.policy.control;
        if (auto queued =
                QueueFrame(peer, {control.protocol, control.id, control.schema, control.version, {}, {}, 0, payload.Value()}, tick);
            queued.HasError())
            return queued;
        peer.pending = record;
        return Result<void>::Success();
    }

    Result<void> NetworkWorldBindingController::Impl::QueueFrame(Peer &peer, MessageEnvelopePayloadView message, const std::uint64_t tick,
                                                                 std::span<std::byte> extendedStorage) const {
        if (peer.frameBytes != 0)
            return Result<void>::Failure(MakeError(NetworkErrors::TransportReliableBackpressure));
        if (peer.sequence == std::numeric_limits<std::uint32_t>::max() ||
            tick > std::numeric_limits<std::uint64_t>::max() - setup.maximumPendingTicks)
            return Result<void>::Failure(MakeError(NetworkErrors::MessageCounterExhausted));
        message.sequence = MessageSequenceNumber{peer.sequence + 1};
        const auto *negotiation = peer.lifecycle ? peer.lifecycle->Negotiation() : nullptr;
        if (!transport || !negotiation)
            return Result<void>::Failure(MakeError(NetworkErrors::AdmissionBindingInvalid));
        MessageEnvelopeLimits limits;
        const auto buffer = extendedStorage.empty() ? std::span<std::byte>{peer.frame} : extendedStorage;
        limits.maximumFrameBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
            {buffer.size(), transport->Capabilities().maximumMessageBytes, negotiation->transport.maximumMessageBytes}));
        // This writer accepts one synchronous extension-free payload, not the general decoder defaults.
        limits.maximumPayloadBytes = message.payload.size();
        limits.maximumFields = 0;
        limits.maximumExtensionBytes = 0;
        const auto encoded = EncodeMessageEnvelopePayloadInto(message, *setup.codecs, buffer, limits);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        peer.frameBytes = encoded.Value();
        peer.deadline = tick + setup.maximumPendingTicks;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::QueueControl */
    Result<void> NetworkWorldBindingController::QueueControl(const ConnectionHandle connection, const NetworkOperationGeneration generation,
                                                             const MessageEnvelopePayloadView &message, const Runtime::RuntimePhase phase) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        const auto &policy = impl_->setup.policy;
        const auto matches = [&message](const MessageIdentityDescriptor &identity) {
            return message.protocol == identity.protocol && message.message == identity.id;
        };
        if (!impl_->dispatcher || !impl_->dispatcher->HasLiveHandler(message.protocol, message.message, MessageTrafficClass::Control) ||
            phase != Runtime::RuntimePhase::NetworkFlush || message.sequence.Value() != 0 || matches(policy.control) ||
            (policy.prefabLifecycle && matches(policy.prefabLifecycle->control)) ||
            (policy.authoredScene && matches(policy.authoredScene->control)) ||
            (policy.initialState && matches(policy.initialState->control)))
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        auto peer = std::ranges::find_if(impl_->peers, [connection, generation](const Impl::Peer &slot) {
            return slot.lifecycle && slot.connection == connection && slot.generation == generation;
        });
        if (peer == impl_->peers.end())
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (auto admitted = impl_->ValidatePeer(*peer->lifecycle, generation, impl_->nowTick); admitted.HasError())
            return admitted;
        return impl_->QueueFrame(*peer, message, impl_->nowTick);
    }

    Result<void> NetworkWorldBindingController::Impl::Send(Peer &peer, const std::uint64_t tick) {
        if (peer.frameBytes == 0)
            return Result<void>::Success();
        if (tick >= peer.deadline)
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        const auto sent = Detail::InvokeAdmissionProvider([this, &peer] {
            const auto buffer = peer.prefabPending || peer.authoredPending || peer.initialFramePending
                                    ? std::span<std::byte>{peer.extendedFrame}
                                    : std::span<std::byte>{peer.frame};
            return transport->Send(peer.connection, setup.controlChannel, buffer.first(peer.frameBytes), DeliveryPolicy::ReliableOrdered);
        });
        if (stopped)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeShuttingDown));
        if (!sent)
            return Result<void>::Failure(MakeError(NetworkErrors::TransportNativeUnavailable));
        if (sent->HasError()) {
            const auto &error = sent->ErrorValue();
            if (error.domain.Value() == NetworkErrors::TransportReliableBackpressure.domain.Value() &&
                error.code.Value() == NetworkErrors::TransportReliableBackpressure.code.Value())
                return Result<void>::Success();  // Exact encoded sequence/deadline remain reserved.
            return Result<void>::Failure(error);
        }
        ++peer.sequence;
        if (peer.prefabPending)
            CompletePrefabSend(peer);
        if (peer.authoredPending)
            CompleteAuthoredSend(peer);
        if (peer.initialFramePending)
            CompleteInitialSend(peer);
        if (peer.pending)
            CompleteBindingSend(peer, tick);
        peer.frameBytes = 0;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::CompleteBindingSend */
    void NetworkWorldBindingController::Impl::CompleteBindingSend(Peer &peer, std::uint64_t tick) const noexcept {
        const auto &binding = *peer.pending;
        if (binding.kind == NetworkWorldBindingKind::PrepareTravel) {
            if (peer.sent != peer.pending)
                peer.preparedDeadline = peer.deadline;
        } else {
            peer.establishedWorld = binding.worldSession;
            if (setup.policy.initialState) {
                peer.initial = {};
                peer.initial.stamp = {binding.worldSession, binding.remoteEpoch, 0};
                const auto remaining = std::numeric_limits<std::uint64_t>::max() - tick;
                peer.initial.deadline = tick + std::min(setup.maximumPendingTicks, remaining);
            }
            if (peer.authoredSent.complete && peer.authoredSent.session == peer.establishedWorld)
                peer.authoredPublished = peer.establishedWorld;
        }
        peer.sent = binding;
        peer.pending.reset();
    }

    void NetworkWorldBindingController::Impl::Retire(Peer &peer, const std::uint64_t tick) noexcept {
        // Foreign cleanup is contained without allocating a second error or replacing a pinned terminal.
        if (!peer.lifecycle)
            return;
        (void)Detail::InvokeAdmissionProvider([&peer, tick] {
            return peer.lifecycle->FailTransport(peer.connection, peer.generation, NetworkFailureKind::TransportUnavailable, tick);
        });
        if (dispatcher)
            dispatcher->RevokeSession(peer.connection);
        // Canonical admission owns native Close. This projection only retires its
        // borrowed peer/dispatcher/outbox; it must not duplicate foreign cleanup.
        peer.lifecycle.reset();
        peer.pending.reset();
        peer.prefabPending.reset();
        peer.authoredPending.reset();
        peer.initial = {};
        peer.initialFramePending = false;
        peer.knownPrefabs.clear();
        peer.frameBytes = 0;
    }

    /** @copydoc NetworkWorldBindingController::Reconcile */
    Result<void> NetworkWorldBindingController::Reconcile(const std::uint64_t nowTick) {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (nowTick < impl_->nowTick) {
            Shutdown();
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        }
        impl_->nowTick = nowTick;
        impl_->worlds.Reconcile(nowTick);
        for (auto &peer : impl_->peers) {
            if (peer.lifecycle)
                (void)peer.lifecycle->Expire(nowTick);
            if (peer.lifecycle && peer.lifecycle->AdmitGameplay(peer.connection, peer.generation, nowTick).HasError())
                impl_->Retire(peer, nowTick);
            else if (peer.lifecycle && !peer.initial.confirmed && peer.initial.deadline != 0 && nowTick >= peer.initial.deadline) {
                peer.failure = MakeError(NetworkErrors::SessionTimedOut);
                impl_->Retire(peer, nowTick);
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueWorldUpdate */
    Result<void> NetworkWorldBindingController::Impl::QueueWorldUpdate(Peer &peer, const NetworkWorldBindingRecord *prepared,
                                                                       const NetworkWorldBindingRecord *current,
                                                                       const std::uint64_t tick) const {
        // A peer joining during travel receives the published world first.
        if (!peer.sendsWorld)
            return Result<void>::Success();
        const auto *record = current;
        if (peer.sent && prepared && current && AuthoredSent(peer, current->worldSession) && InitialConfirmed(peer))
            record = prepared;
        if (const bool advances =
                record && (!peer.sent || record->worldSession > peer.sent->worldSession ||
                           (record->worldSession == peer.sent->worldSession && record->kind == NetworkWorldBindingKind::CommitTravel &&
                            peer.sent->kind == NetworkWorldBindingKind::PrepareTravel));
            peer.frameBytes == 0 && advances) {
            auto selected = *record;
            if (!peer.sent) {
                selected.kind = NetworkWorldBindingKind::Initial;
                selected.expectedPriorBinding = {};
            }
            return Queue(peer, selected, tick);
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Flush */
    Result<void> NetworkWorldBindingController::Flush(const std::uint64_t nowTick) {
        if (auto checked = Reconcile(nowTick); checked.HasError())
            return checked;
        const Impl::BusyScope busy{impl_->busy};
        if (!impl_->transport)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        const auto *prepared = impl_->worlds.PreparedServerBinding();
        const auto *current = impl_->worlds.ServerBinding();
        for (auto &peer : impl_->peers) {
            if (!peer.lifecycle)
                continue;
            if (peer.authoredPending && !impl_->worlds.AuthorityAuthored(peer.authoredPending->session)) {
                peer.authoredPending.reset();
                peer.frameBytes = 0;
            }
            if (auto queued = impl_->QueueWorldUpdate(peer, prepared, current, nowTick); queued.HasError())
                return queued;  // Local encoding/capacity error precedes any new publication.
            if (auto queued = impl_->QueueAuthoredUpdate(peer, nowTick); queued.HasError()) {
                peer.failure = queued.ErrorValue();
                impl_->Retire(peer, nowTick);
                continue;
            }
            if (auto queued = impl_->QueuePrefabUpdate(peer, prepared, current, nowTick); queued.HasError()) {
                peer.failure = queued.ErrorValue();
                impl_->Retire(peer, nowTick);
                continue;
            }
            if (auto queued = impl_->QueueInitial(peer, current, nowTick); queued.HasError()) {
                if (impl_->stopped)
                    return queued;
                peer.failure = queued.ErrorValue();
                impl_->Retire(peer, nowTick);
                continue;
            }
            if (auto sent = impl_->Send(peer, nowTick); sent.HasError()) {
                if (impl_->stopped)
                    return sent;
                peer.failure = sent.ErrorValue();
                impl_->Retire(peer, nowTick);  // One failed peer never retires another world or peer.
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Network
