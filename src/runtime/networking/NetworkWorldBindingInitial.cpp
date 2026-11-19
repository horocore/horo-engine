#include "NetworkProviderBoundary.h"
#include "NetworkWorldBindingState.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Produces a typed failure for the current peer without altering other world publications. */
        Result<void> RejectInitial(const ErrorCodeDescriptor &reason = NetworkErrors::ReplicationWorldStale) {
            return Result<void>::Failure(MakeError(reason));
        }

        /** @brief Checks complete ordered object coverage and the configured aggregate wire ceiling. */
        bool MatchesCapture(const ReplicationInitialStateSnapshot &snapshot, const ReplicationWorldReadLease &lease, std::size_t budget) {
            if (!lease.HasCurrentMapping() || snapshot.session != lease.Descriptor().session ||
                snapshot.authority != lease.Descriptor().authority || snapshot.tick == 0)
                return false;
            const auto object = [](const ReplicationInitialStateRecord &row) {
                return row.ownership.object;
            };
            if (!std::ranges::equal(snapshot.records, lease.Mapping().Entries(), {}, object, &NetworkObjectMappingEntry::object))
                return false;
            for (const auto &object : snapshot.records) {
                if (object.wire.size() > budget)
                    return false;
                budget -= object.wire.size();
            }
            return true;
        }
    }  // namespace

    /** @copydoc NetworkWorldBindingController::InitialClientOwnership */
    Result<ReplicationInitialOwnership> NetworkWorldBindingController::InitialClientOwnership(Runtime::SceneRuntimeId scene,
                                                                                              NetworkSessionGeneration session,
                                                                                              NetworkObjectId object) const {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return Result<ReplicationInitialOwnership>::Failure(checked.ErrorValue());
        if (!impl_->setup.policy.initialState)
            return Result<ReplicationInitialOwnership>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        return impl_->worlds.InitialOwnership(scene, session, object);
    }

    /** @copydoc NetworkWorldBindingController::Impl::InitialConfirmed */
    bool NetworkWorldBindingController::Impl::InitialConfirmed(const Peer &peer) const noexcept {
        if (!setup.policy.initialState)
            return true;
        const auto *world = peer.sendsWorld ? worlds.ServerBinding() : worlds.ClientBinding();
        return world && peer.initial.confirmed && peer.initial.stamp.session == world->worldSession &&
               peer.initial.stamp.authority == world->remoteEpoch;
    }

    /** @copydoc NetworkWorldBindingController::Impl::AcceptInitial */
    Result<void> NetworkWorldBindingController::Impl::AcceptInitial(const MessageEnvelope &message, const InboundMessageContext &context) {
        const auto found = std::ranges::find_if(peers, [&context](const Peer &peer) {
            return peer.lifecycle == context.session && peer.connection == context.connection && peer.generation == context.generation;
        });
        if (!context.session || found == peers.end() || context.delivery != DeliveryPolicy::ReliableOrdered ||
            context.cancellation.IsCancellationRequested())
            return RejectInitial(NetworkErrors::GameplayDispatchRejected);
        if (auto active = ValidatePeer(*found->lifecycle, found->generation, context.ownerTick); active.HasError())
            return active;
        if (!found->sendsWorld)
            return ReceiveInitial(*found, message, context);
        return AcceptInitialReceipt(*found, message.payload, context.ownerTick);
    }

    /** @copydoc NetworkWorldBindingController::Impl::AcceptInitialReceipt */
    Result<void> NetworkWorldBindingController::Impl::AcceptInitialReceipt(Peer &peer, std::span<const std::byte> payload,
                                                                           std::uint64_t tick) {
        const auto receipt = DecodeNetworkInitialStateReceipt(payload);
        if (receipt.HasError())
            return Result<void>::Failure(receipt.ErrorValue());
        if (!peer.initial.sent || peer.initial.stamp != receipt.Value() || tick >= peer.initial.deadline)
            return RejectInitial();
        peer.initial.confirmed = true;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::InitialAuthority */
    ReplicationInboundAuthority NetworkWorldBindingController::Impl::InitialAuthority(const Peer &peer) const {
        const auto &route = setup.policy.initialState->control;
        return {peer.lifecycle,
                peer.connection,
                peer.generation,
                peer.initial.clientScene,
                peer.initial.stamp.session,
                peer.initial.stamp.authority,
                peer.initial.recipient,
                route.protocol,
                route.id};
    }

    /** @copydoc NetworkWorldBindingController::Impl::OpenInitialInbox */
    Result<void> NetworkWorldBindingController::Impl::OpenInitialInbox(Peer &peer, NetworkInitialStateStamp stamp,
                                                                       Runtime::SceneRuntimeId scene, std::uint64_t tick) {
        if (peer.initial.stamp.session.IsValid() && (!peer.initial.confirmed || peer.initial.stamp.session >= stamp.session))
            return RejectInitial();
        if (tick > std::numeric_limits<std::uint64_t>::max() - setup.maximumPendingTicks)
            return RejectInitial(NetworkErrors::MessageCounterExhausted);
        auto recipient = Detail::InvokeAdmissionProvider([&] {
            return setup.initialStateHost->ClientRecipient(*peer.lifecycle);
        });
        if (stopped || !recipient)
            return RejectInitial(NetworkErrors::NetworkModeUnavailable);
        if (recipient->HasError())
            return Result<void>::Failure(recipient->ErrorValue());
        if (!recipient->Value().IsValid())
            return RejectInitial(NetworkErrors::ReplicationDescriptorIncompatible);
        peer.initial = {};
        peer.initial.stamp = stamp;
        peer.initial.deadline = tick + setup.maximumPendingTicks;
        peer.initial.clientScene = scene;
        peer.initial.recipient = recipient->Value();
        try {
            const auto &policy = *setup.policy.initialState;
            peer.initial.inbox =
                std::make_unique<NetworkInitialStateInbox>(InitialAuthority(peer), policy.control, policy.maximumWireBytes);
        } catch (const std::bad_alloc &) {
            peer.initial = {};
            return RejectInitial(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::ReceiveInitial */
    Result<void> NetworkWorldBindingController::Impl::ReceiveInitial(Peer &peer, const MessageEnvelope &message,
                                                                     const InboundMessageContext &context) {
        const auto stamp = ReadNetworkInitialStateStamp(message.payload);
        if (stamp.HasError())
            return Result<void>::Failure(stamp.ErrorValue());
        const auto scene = worlds.AuthorizeInitial(stamp.Value().session, stamp.Value().authority, context);
        if (scene.HasError())
            return Result<void>::Failure(scene.ErrorValue());
        const bool newTransfer = !peer.initial.inbox;
        if (newTransfer) {
            if (auto opened = OpenInitialInbox(peer, stamp.Value(), scene.Value(), context.ownerTick); opened.HasError())
                return opened;
        }
        if (context.ownerTick >= peer.initial.deadline)
            return RejectInitial(NetworkErrors::SessionTimedOut);
        auto admitted = peer.initial.inbox->HandleAdmitted(context, message);
        if (admitted.HasValue())
            peer.initial.receivedTick = context.ownerTick;
        else if (newTransfer)
            peer.initial = {};
        return admitted;
    }

    /** @copydoc NetworkWorldBindingController::Impl::CaptureInitial */
    Result<void> NetworkWorldBindingController::Impl::CaptureInitial(Peer &peer, const NetworkWorldBindingRecord &current,
                                                                     std::uint64_t tick) {
        if (peer.prefabSession != current.worldSession || peer.prefabRevision == 0)
            return Result<void>::Success();
        const auto world = worlds.AcquireInitialAuthority(tick);
        if (world.HasError())
            return Result<void>::Failure(world.ErrorValue());
        const auto budget = setup.policy.initialState->maximumWireBytes;
        auto capture = Detail::InvokeAdmissionProvider([&] {
            return setup.initialStateHost->Capture(world.Value().mapping, world.Value().lifecycle, *peer.lifecycle, budget);
        });
        if (stopped || !capture)
            return RejectInitial(NetworkErrors::NetworkModeUnavailable);
        if (capture->HasError())
            return Result<void>::Failure(capture->ErrorValue());
        if (!capture->Value())
            return Result<void>::Success();
        auto &snapshot = *capture->Value();
        if (!MatchesCapture(snapshot, world.Value().mapping, budget))
            return RejectInitial(NetworkErrors::ReplicationDescriptorIncompatible);
        peer.initial.stamp.tick = snapshot.tick;
        peer.initial.snapshot.emplace(std::move(snapshot));
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueInitialFrame */
    Result<void> NetworkWorldBindingController::Impl::QueueInitialFrame(Peer &peer, std::span<const std::byte> payload,
                                                                        std::uint64_t tick) {
        try {
            peer.extendedFrame.resize(MessageEnvelopeFixedHeaderBytes + payload.size());
        } catch (const std::bad_alloc &) {
            return RejectInitial(NetworkErrors::MessageEnvelopeCapacityExceeded);
        }
        const auto &route = setup.policy.initialState->control;
        const MessageEnvelopePayloadView frame{route.protocol, route.id, route.schema, route.version, {}, {}, 0, payload};
        auto queued = QueueFrame(peer, frame, tick, peer.extendedFrame);
        if (queued.HasValue())
            peer.initialFramePending = true;
        return queued;
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueInitial */
    Result<void> NetworkWorldBindingController::Impl::QueueInitial(Peer &peer, const NetworkWorldBindingRecord *current,
                                                                   std::uint64_t tick) {
        if (!setup.policy.initialState || peer.frameBytes != 0 || peer.initial.confirmed)
            return Result<void>::Success();
        auto &stream = peer.initial;
        if (!peer.sendsWorld)
            return QueueInitialReceipt(peer, tick);
        if (!stream.CanSend(current))
            return Result<void>::Success();
        if (!stream.snapshot) {
            if (auto captured = CaptureInitial(peer, *current, tick); captured.HasError() || !stream.snapshot)
                return captured;
        }
        return QueueInitialPayload(peer, tick);
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueInitialReceipt */
    Result<void> NetworkWorldBindingController::Impl::QueueInitialReceipt(Peer &peer, std::uint64_t tick) {
        if (!peer.initial.published)
            return Result<void>::Success();
        auto receipt = EncodeNetworkInitialStateReceipt(peer.initial.stamp);
        return receipt.HasValue() ? QueueInitialFrame(peer, receipt.Value(), tick) : Result<void>::Failure(receipt.ErrorValue());
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueueInitialPayload */
    Result<void> NetworkWorldBindingController::Impl::QueueInitialPayload(Peer &peer, std::uint64_t tick) {
        auto &stream = peer.initial;
        const auto &snapshot = *stream.snapshot;
        if (!stream.manifestQueued) {
            auto manifest = EncodeNetworkInitialStateManifest(snapshot);
            if (manifest.HasError())
                return Result<void>::Failure(manifest.ErrorValue());
            auto queued = QueueInitialFrame(peer, manifest.Value(), tick);
            stream.manifestQueued = queued.HasValue();
            return queued;
        }
        const auto &route = setup.policy.initialState->control;
        const auto codec = setup.codecs->FindCodec(route.protocol, route.id);
        const auto frameBytes =
            std::min(transport->Capabilities().maximumMessageBytes, peer.lifecycle->Negotiation()->transport.maximumMessageBytes);
        if (!codec || frameBytes <= MessageEnvelopeFixedHeaderBytes)
            return RejectInitial(NetworkErrors::MessageEnvelopeCapacityExceeded);
        const auto limit = std::min<std::uint64_t>(codec->maximumPayloadBytes, frameBytes - MessageEnvelopeFixedHeaderBytes);
        auto segment = EncodeNetworkInitialStateSegment(snapshot, stream.record, stream.offset, static_cast<std::size_t>(limit));
        if (segment.HasError())
            return Result<void>::Failure(segment.ErrorValue());
        auto queued = QueueInitialFrame(peer, segment.Value().payload, tick);
        if (queued.HasError())
            return queued;
        stream.offset = segment.Value().nextOffset;
        if (stream.offset == snapshot.records[stream.record].wire.size()) {
            ++stream.record;
            stream.offset = 0;
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::CompleteInitialSend */
    void NetworkWorldBindingController::Impl::CompleteInitialSend(Peer &peer) noexcept {
        peer.initialFramePending = false;
        auto &stream = peer.initial;
        if (!peer.sendsWorld) {
            stream.confirmed = true;
        } else if (stream.snapshot && stream.manifestQueued && stream.record == stream.snapshot->records.size()) {
            stream.sent = true;
            stream.snapshot.reset();
        }
    }

    /** @copydoc NetworkWorldBindingController::Impl::ApplyClientInitial */
    Result<void> NetworkWorldBindingController::Impl::ApplyClientInitial(Peer &peer, std::uint64_t localSimulationTick,
                                                                         const CancellationToken &cancellation) {
        auto &stream = peer.initial;
        const auto count = stream.inbox ? stream.inbox->ReadyRecordCount() : std::nullopt;
        if (!count)
            return Result<void>::Success();
        const auto tick = std::max(nowTick, stream.receivedTick);
        if (tick >= stream.deadline)
            return RejectInitial(NetworkErrors::SessionTimedOut);
        auto *world = worlds.InitialReceiver(stream.stamp.session, *count);
        if (!world)
            return Result<void>::Success();
        const ReplicationWorldWorkRequest request{stream.clientScene, stream.stamp.session,
                                                  Runtime::RuntimePhase::CommitDeferredLifecycleChanges, localSimulationTick, cancellation};
        return CommitClientInitial(peer, *world, request, tick);
    }

    /** @copydoc NetworkWorldBindingController::Impl::CommitClientInitial */
    Result<void> NetworkWorldBindingController::Impl::CommitClientInitial(Peer &peer, ReplicationWorldLifecycle &world,
                                                                          const ReplicationWorldWorkRequest &request, std::uint64_t tick) {
        auto &stream = peer.initial;
        const auto authority = InitialAuthority(peer);
        auto ownership = stream.inbox->ReadOwnership(tick);
        if (ownership.HasError())
            return Result<void>::Failure(ownership.ErrorValue());
        auto target = PrepareInitialTarget(authority, world, ownership.Value(), tick);
        if (target.HasError())
            return Result<void>::Failure(target.ErrorValue());
        auto applied = stream.inbox->ApplyCompleted(*target.Value().receiver, request, tick);
        if (applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        if (!applied.Value().initialPublished)
            return Result<void>::Success();
        if (!worlds.PublishInitialOwnership(stream.stamp.session, std::move(ownership).Value()))
            return RejectInitial(NetworkErrors::ReplicationWorldStale);
        stream.published = true;
        stream.inbox.reset();
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::PrepareInitialTarget */
    Result<NetworkInitialStateTarget> NetworkWorldBindingController::Impl::PrepareInitialTarget(
        const ReplicationInboundAuthority &authority, ReplicationWorldLifecycle &world,
        std::span<const ReplicationInitialOwnership> ownership, std::uint64_t tick) {
        using Outcome = Result<NetworkInitialStateTarget>;
        auto prepared = Detail::InvokeAdmissionProvider([&] {
            return setup.initialStateHost->PrepareReceiver(authority, world, ownership, tick);
        });
        if (stopped || !prepared)
            return Outcome::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (prepared->HasError())
            return Outcome::Failure(prepared->ErrorValue());
        if (!prepared->Value().receiver || !prepared->Value().owner)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        return std::move(*prepared);
    }

    /** @copydoc NetworkWorldBindingController::Impl::ApplyInitial */
    void NetworkWorldBindingController::Impl::ApplyInitial(std::uint64_t localSimulationTick, const CancellationToken &cancellation) {
        for (auto &peer : peers) {
            if (!peer.lifecycle || peer.sendsWorld)
                continue;
            if (auto applied = ApplyClientInitial(peer, localSimulationTick, cancellation); applied.HasError() && !stopped) {
                peer.failure = applied.ErrorValue();
                Retire(peer, std::max(nowTick, peer.initial.receivedTick));
            }
        }
    }
}  // namespace Horo::Network
