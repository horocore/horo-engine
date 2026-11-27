#include "NetworkProviderBoundary.h"
#include "NetworkWorldBindingState.h"
#include "ReplicationStateCodecInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Network {
    namespace {
        /** @brief Reads the fixed full-record route identity before sending or staging a Snapshot. */
        struct UpdateIdentity final {
            NetworkObjectId object;
            NetworkSessionGeneration session;
            ReplicationRoleRevision role;
            NetworkPeerId recipient;
            std::uint64_t descriptorGeneration{};
            std::uint64_t schema{};
            ReplicationSchemaVersion version;
            std::uint64_t tick{};
        };

        /** @brief Rejects delta framing on the baseline-free route before any owner callback. */
        std::optional<UpdateIdentity> ReadFullStateIdentity(std::span<const std::byte> bytes) {
            if (bytes.size() < StateCodecDetail::HeaderBytes)
                return std::nullopt;
            StateCodecDetail::Reader reader{bytes};
            if (!reader.Match(StateCodecDetail::Magic, 4) || !reader.Match(1, 1) || !reader.Match(0, 3))
                return std::nullopt;
            std::uint64_t session{}, descriptor{}, role{}, recipient{}, epoch{}, slot{}, generation{}, schema{}, major{}, minor{};
            if (!reader.Read(8, session) || !reader.Read(8, descriptor) || !reader.Read(8, role) || !reader.Read(8, recipient) ||
                !reader.Read(8, epoch) || !reader.Read(8, slot) || !reader.Read(4, generation) || !reader.Read(8, schema) ||
                !reader.Read(2, major) || !reader.Read(2, minor))
                return std::nullopt;
            std::uint64_t tick{}, publication{};
            if (!reader.Read(8, tick) || !reader.Read(8, publication) || !reader.Match(0, 8) || !reader.Match(0, 8) || tick == 0 ||
                publication == 0 || descriptor == 0)
                return std::nullopt;
            const auto authority = ReplicationAuthorityEpoch::Create(epoch);
            const auto world = NetworkSessionGeneration::Create(session);
            const auto revision = ReplicationRoleRevision::Create(role);
            const auto target = NetworkPeerId::Create(recipient);
            if (authority.HasError() || world.HasError() || revision.HasError() || target.HasError())
                return std::nullopt;
            const auto object = NetworkObjectId::Create(authority.Value(), slot, static_cast<std::uint32_t>(generation));
            if (object.HasError())
                return std::nullopt;
            return UpdateIdentity{object.Value(),
                                  world.Value(),
                                  revision.Value(),
                                  target.Value(),
                                  descriptor,
                                  schema,
                                  {static_cast<std::uint16_t>(major), static_cast<std::uint16_t>(minor)},
                                  tick};
        }

        /** @brief Verifies every frame fits the opted-in recipient's exact authenticated admission and bounded roster. */
        bool MatchesUpdateBatch(const NetworkStateUpdateBatch &batch, const NetworkStateUpdatePolicy &policy,
                                const ReplicationWorldReadLease &world, const NetworkInitialUpdateCohort &cohort,
                                std::size_t maximumFrame) {
            if (batch.simulationTick == 0 || batch.records.size() > policy.maximumMessagesPerTick || !world.HasCurrentMapping())
                return false;
            auto remaining = policy.maximumBytesPerTick;
            const auto mappings = world.Mapping().Entries();
            std::optional<NetworkObjectId> previous;
            for (const auto &record : batch.records) {
                const auto bytes = record.size() + MessageEnvelopeFixedHeaderBytes;
                if (bytes > remaining || bytes > maximumFrame)
                    return false;
                remaining -= bytes;
                const auto identity = ReadFullStateIdentity(record);
                if (!identity || identity->tick != batch.simulationTick || identity->session != world.Descriptor().session ||
                    identity->object.Epoch() != world.Descriptor().authority || (previous && *previous >= identity->object))
                    return false;
                if (identity->recipient != cohort.recipient || identity->descriptorGeneration != cohort.descriptorGeneration)
                    return false;
                const auto grant = std::ranges::lower_bound(cohort.ownership, identity->object, {}, &ReplicationInitialOwnership::object);
                const auto mapping = std::ranges::lower_bound(mappings, identity->object, {}, &NetworkObjectMappingEntry::object);
                if (grant == cohort.ownership.end() || grant->object != identity->object || grant->revision != identity->role ||
                    mapping == mappings.end() || mapping->object != identity->object ||
                    mapping->provenance.schema.Value() != identity->schema || mapping->provenance.schemaVersion != identity->version)
                    return false;
                previous = identity->object;
            }
            return true;
        }
    }  // namespace

    /** @copydoc NetworkWorldBindingController::Impl::RetainUpdateCohort */
    Result<void> NetworkWorldBindingController::Impl::RetainUpdateCohort(Peer &peer, const ReplicationInitialStateSnapshot &snapshot) {
        if (!setup.policy.stateUpdates || snapshot.records.empty())
            return Result<void>::Success();
        try {
            const auto first = ReadFullStateIdentity(snapshot.records.front().wire);
            if (!first)
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
            auto cohort = std::make_shared<NetworkInitialUpdateCohort>();
            cohort->recipient = first->recipient;
            cohort->descriptorGeneration = first->descriptorGeneration;
            cohort->ownership.reserve(snapshot.records.size());
            for (const auto &row : snapshot.records) {
                const auto identity = ReadFullStateIdentity(row.wire);
                if (!identity || identity->object != row.ownership.object || identity->role != row.ownership.revision ||
                    identity->recipient != cohort->recipient || identity->descriptorGeneration != cohort->descriptorGeneration ||
                    identity->session != snapshot.session || identity->tick != snapshot.tick)
                    return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
                cohort->ownership.push_back(row.ownership);
            }
            peer.initialUpdateCohort = std::move(cohort);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::PrepareStateUpdates */
    Result<NetworkInitialStateTarget> NetworkWorldBindingController::Impl::PrepareStateUpdates(
        const ReplicationInboundAuthority &initial, ReplicationWorldLifecycle &world,
        std::span<const ReplicationInitialOwnership> ownership, std::uint64_t now) {
        using Target = Result<NetworkInitialStateTarget>;
        if (!setup.policy.stateUpdates)
            return Target::Success({});
        auto authority = initial;
        authority.message = setup.policy.stateUpdates->snapshot.id;
        auto prepared = Detail::InvokeAdmissionProvider([&] {
            return setup.stateUpdateHost->PrepareUpdates(authority, world, ownership, now);
        });
        if (stopped || detached || !prepared)
            return Target::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (prepared->HasError())
            return std::move(*prepared);
        if (!prepared->Value().owner || !prepared->Value().receiver)
            return Target::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        return std::move(*prepared);
    }

    /** @copydoc NetworkWorldBindingController::Impl::AcceptStateUpdate */
    Result<void> NetworkWorldBindingController::Impl::AcceptStateUpdate(const MessageEnvelope &message,
                                                                        const InboundMessageContext &context) {
        const auto found = std::ranges::find_if(peers, [&](const Peer &peer) {
            return !peer.sendsWorld && peer.lifecycle == context.session && peer.connection == context.connection &&
                   peer.generation == context.generation;
        });
        if (!context.session || found == peers.end() || context.delivery != DeliveryPolicy::UnreliableUnordered ||
            !InitialConfirmed(*found))
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        const auto identity = ReadFullStateIdentity(message.payload);
        if (!identity || identity->recipient != found->initial.recipient || identity->session != found->initial.stamp.session ||
            identity->tick <= found->initial.stamp.tick)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        const auto *target = worlds.ClientStateUpdates(identity->session);
        if (!target)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        const auto receiver = target->receiver;
        return receiver->HandleAdmitted(context, message);
    }

    /** @copydoc NetworkWorldBindingController::Impl::ApplyStateUpdates */
    void NetworkWorldBindingController::Impl::ApplyStateUpdates(std::uint64_t localSimulationTick, const CancellationToken &cancellation) {
        if (!setup.policy.stateUpdates || cancellation.IsCancellationRequested() || localSimulationTick == 0)
            return;
        for (auto &peer : peers) {
            if (stopped || detached)
                return;
            if (!peer.lifecycle || peer.sendsWorld || !InitialConfirmed(peer))
                continue;
            const auto session = peer.initial.stamp.session;
            const auto *target = worlds.ClientStateUpdates(session);
            if (!target || !target->receiver->HasPendingUpdates())
                continue;
            const auto receiver = target->receiver;
            const ReplicationWorldWorkRequest request{peer.initial.clientScene, session,
                                                      Runtime::RuntimePhase::CommitDeferredLifecycleChanges, localSimulationTick,
                                                      cancellation};
            const auto applied = receiver->ApplyAtSafePoint(request, nowTick);
            if (applied.HasError() && !stopped)
                peer.failure = applied.ErrorValue();
        }
    }

    /** @copydoc NetworkWorldBindingController::Impl::FlushStateUpdates */
    Result<void> NetworkWorldBindingController::Impl::FlushStateUpdates(Peer &peer, std::uint64_t now) {
        if (!setup.policy.stateUpdates || !peer.sendsWorld || !peer.initialUpdateCohort || !InitialConfirmed(peer) ||
            peer.lastUpdateFlush == now || !worlds.ReadyForPeer(true))
            return Result<void>::Success();
        peer.lastUpdateFlush = now;
        const auto world = worlds.AcquireInitialAuthority(now);
        if (world.HasError())
            return Result<void>::Failure(world.ErrorValue());
        if (auto admitted = ValidatePeer(*peer.lifecycle, peer.generation, now); admitted.HasError())
            return admitted;
        const auto *authenticated = peer.lifecycle->Authentication();
        if (!authenticated)
            return Result<void>::Failure(MakeError(NetworkErrors::AuthenticationRejected));
        const auto evidence = *authenticated;
        const auto activePeer = peer.lifecycle;
        const auto cohort = peer.initialUpdateCohort;
        const auto &policy = *setup.policy.stateUpdates;
        const auto *negotiation = peer.lifecycle->Negotiation();
        const auto maximumFrame = std::min(negotiation->transport.maximumMessageBytes, transport->Capabilities().maximumMessageBytes);
        auto captured = Detail::InvokeAdmissionProvider([&] {
            return setup.stateUpdateHost->CaptureUpdates(world.Value().mapping, world.Value().lifecycle, *activePeer, cohort->ownership,
                                                         now, policy);
        });
        if (stopped || detached || !captured)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (captured->HasError())
            return Result<void>::Failure(std::move(*captured).ErrorValue());
        if (!captured->Value())
            return Result<void>::Success();
        const auto &batch = *captured->Value();
        if (batch.simulationTick <= std::max(peer.lastUpdateTick, peer.initial.stamp.tick))
            return Result<void>::Success();
        if (!MatchesUpdateBatch(batch, policy, world.Value().mapping, *cohort, maximumFrame))
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        if (auto admitted = ValidatePeer(*peer.lifecycle, peer.generation, now); admitted.HasError())
            return admitted;
        if (!peer.lifecycle->Authentication() || *peer.lifecycle->Authentication() != evidence ||
            !world.Value().mapping.HasCurrentMapping())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        peer.lastUpdateTick = batch.simulationTick;
        std::vector<std::byte> buffer;
        for (const auto &payload : batch.records) {
            if (peer.updateSequence == std::numeric_limits<std::uint32_t>::max())
                return Result<void>::Failure(MakeError(NetworkErrors::MessageCounterExhausted));
            const auto &route = policy.snapshot;
            const MessageEnvelopePayloadView
                message{route.protocol, route.id, route.schema, route.version, MessageSequenceNumber{++peer.updateSequence}, {}, 0,
                        payload};
            MessageEnvelopeLimits limits;
            limits.maximumFrameBytes = maximumFrame;
            limits.maximumPayloadBytes = payload.size();
            limits.maximumFields = 0;
            limits.maximumExtensionBytes = 0;
            try {
                buffer.resize(MessageEnvelopeFixedHeaderBytes + payload.size());
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
            }
            auto encoded = EncodeMessageEnvelopePayloadInto(message, *setup.codecs, buffer, limits);
            if (encoded.HasError())
                return Result<void>::Failure(std::move(encoded).ErrorValue());
            const auto sent = Detail::InvokeAdmissionProvider([&] {
                return transport->Send(peer.connection, policy.channel, std::span{buffer}.first(encoded.Value()),
                                       DeliveryPolicy::UnreliableUnordered);
            });
            if (stopped || detached || !sent)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
            if (sent->HasError())
                return Result<void>::Failure(std::move(*sent).ErrorValue());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Network
