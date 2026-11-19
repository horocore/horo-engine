#include "NetworkWorldBindingState.h"

#include <algorithm>
#include <new>

namespace Horo::Network {
    namespace {
        /** @brief Retains exact authenticated group evidence and a weak client-world route. */
        class ReplicaPrefabPublication final : public Runtime::SceneGroupPublication {
        public:
            ReplicaPrefabPublication(std::weak_ptr<NetworkWorldBindingController> binding, Runtime::SceneRuntimeId scene,
                                     NetworkPrefabLifecycleRecord spawn, CancellationToken cancellation)
                : binding_(std::move(binding)), scene_(scene), spawn_(std::move(spawn)), cancellation_(std::move(cancellation)) {}

            Result<std::unique_ptr<Runtime::SceneStructuralCandidate>> Prepare(const Runtime::RuntimeSceneView &active,
                                                                               std::span<const Runtime::EntityRef> members,
                                                                               bool creating) const override {
                const auto binding = binding_.lock();
                if (!binding || active.RuntimeId() != scene_)
                    return Result<std::unique_ptr<Runtime::SceneStructuralCandidate>>::Failure(
                        MakeError(NetworkErrors::ReplicationWorldUnavailable));
                return binding->PrepareClientPrefab(scene_, spawn_, members, creating, cancellation_);
            }

        private:
            std::weak_ptr<NetworkWorldBindingController> binding_;
            Runtime::SceneRuntimeId scene_;
            NetworkPrefabLifecycleRecord spawn_;
            CancellationToken cancellation_;
        };
    }  // namespace

    /** @copydoc NetworkAuthorityObjectPublication::Find */
    Result<NetworkObjectId> NetworkAuthorityObjectPublication::Find(Runtime::EntityRef entity) const {
        const auto binding = binding_.lock();
        if (!binding || entity.runtime != scene_)
            return Result<NetworkObjectId>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        return binding->FindAuthorityObject(entity);
    }

    /** @copydoc NetworkClientPrefabInbox::Resolve */
    Result<Runtime::EntityRef> NetworkClientPrefabInbox::Resolve(NetworkSessionGeneration session, NetworkObjectId object) const {
        if (const auto binding = binding_.lock())
            return binding->ResolveClientObject(scene_, session, object);
        return Result<Runtime::EntityRef>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
    }

    /** @copydoc NetworkWorldBindingController::FindAuthorityObject */
    Result<NetworkObjectId> NetworkWorldBindingController::FindAuthorityObject(Runtime::EntityRef entity) const {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return Result<NetworkObjectId>::Failure(checked.ErrorValue());
        return impl_->worlds.FindAuthorityObject(entity);
    }

    /** @copydoc NetworkWorldBindingController::ResolveClientObject */
    Result<Runtime::EntityRef> NetworkWorldBindingController::ResolveClientObject(Runtime::SceneRuntimeId scene,
                                                                                  NetworkSessionGeneration session,
                                                                                  NetworkObjectId object) const {
        if (auto checked = impl_->CheckOwner(); checked.HasError())
            return Result<Runtime::EntityRef>::Failure(checked.ErrorValue());
        return impl_->worlds.ResolveClientObject(scene, session, object);
    }

    /** @copydoc NetworkClientPrefabInbox::Bind */
    Result<std::shared_ptr<const Runtime::SceneGroupPublication>> NetworkClientPrefabInbox::Bind(
        NetworkPrefabLifecycleRecord spawn, const CancellationToken &cancellation) const {
        using Outcome = Result<std::shared_ptr<const Runtime::SceneGroupPublication>>;
        if (binding_.expired() || cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        if (!spawn.spawn)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldInvalid));
        if (auto valid = ValidateNetworkPrefabLifecycle(spawn); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        try {
            return Outcome::Success(std::make_shared<const ReplicaPrefabPublication>(binding_, scene_, std::move(spawn), cancellation));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueuePrefabUpdate */
    Result<void> NetworkWorldBindingController::Impl::QueuePrefabUpdate(Peer &peer, const NetworkWorldBindingRecord *prepared,
                                                                        const NetworkWorldBindingRecord *current,
                                                                        const std::uint64_t tick) {
        if (!setup.policy.prefabLifecycle || !peer.sendsWorld || peer.frameBytes != 0 || prepared || !current ||
            peer.establishedWorld != current->worldSession)
            return Result<void>::Success();
        const auto features = peer.lifecycle->Negotiation()->features.Values();
        if (std::ranges::find(features, setup.policy.prefabLifecycle->feature) == features.end())
            return Result<void>::Success();
        if (peer.prefabSession != current->worldSession) {
            peer.prefabSession = current->worldSession;
            peer.prefabRevision = 0;
            peer.knownPrefabs.clear();
        }
        auto snapshot = worlds.CaptureAuthorityPrefabs();
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        const auto &[revision, records] = snapshot.Value();
        if (peer.prefabRevision == revision)
            return Result<void>::Success();
        const auto retired = std::ranges::find_if(peer.knownPrefabs, [records](NetworkObjectId occurrence) {
            return !std::ranges::binary_search(records, occurrence, {}, &NetworkPrefabLifecycleRecord::occurrence);
        });
        if (retired != peer.knownPrefabs.end())
            return QueuePrefabRecord(peer, {current->worldSession, *retired, std::nullopt}, records.size(), tick);
        const auto spawned = std::ranges::find_if(records, [&peer](const NetworkPrefabLifecycleRecord &record) {
            return !std::ranges::binary_search(peer.knownPrefabs, record.occurrence);
        });
        if (spawned != records.end())
            return QueuePrefabRecord(peer, *spawned, records.size(), tick);
        peer.prefabRevision = revision;
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::QueuePrefabRecord */
    Result<void> NetworkWorldBindingController::Impl::QueuePrefabRecord(Peer &peer, const NetworkPrefabLifecycleRecord &record,
                                                                        std::size_t expectedGroups, const std::uint64_t tick) const {
        const auto payload = EncodeNetworkPrefabLifecycle(record);
        if (payload.HasError())
            return Result<void>::Failure(payload.ErrorValue());
        try {
            // The fixed control slot budget covers the envelope overhead; actual bytes are still bounded by negotiated limits.
            peer.prefabFrame.resize(payload.Value().size() + peer.frame.size());
            if (record.spawn)
                peer.knownPrefabs.reserve(expectedGroups);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
        const auto &identity = setup.policy.prefabLifecycle->control;
        const MessageEnvelopePayloadView message{identity.protocol, identity.id, identity.schema, identity.version, {}, {}, 0,
                                                 payload.Value()};
        if (auto queued = QueueFrame(peer, message, tick, peer.prefabFrame); queued.HasError())
            return queued;
        peer.prefabPending = Peer::PrefabChange{record.occurrence, record.spawn.has_value()};
        return Result<void>::Success();
    }

    /** @copydoc NetworkWorldBindingController::Impl::CompletePrefabSend */
    void NetworkWorldBindingController::Impl::CompletePrefabSend(Peer &peer) noexcept {
        const auto change = *peer.prefabPending;
        const auto found = std::ranges::lower_bound(peer.knownPrefabs, change.occurrence);
        if (change.spawn)
            peer.knownPrefabs.insert(found, change.occurrence);
        else
            peer.knownPrefabs.erase(found);
        peer.prefabPending.reset();
    }
}  // namespace Horo::Network
