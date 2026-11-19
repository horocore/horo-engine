#include "Horo/Network/NetworkErrors.h"
#include "NetworkModeWorldState.h"

#include <algorithm>

namespace Horo::Network::Detail {
    /** @copydoc NetworkModeWorldState::AuthorizeInitial */
    Result<Runtime::SceneRuntimeId> NetworkModeWorldState::AuthorizeInitial(NetworkSessionGeneration session,
                                                                            ReplicationAuthorityEpoch authority,
                                                                            const InboundMessageContext &context) const {
        using Outcome = Result<Runtime::SceneRuntimeId>;
        if (auto admitted = AuthorizeClient(context); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        const auto index = Index(NetworkModeWorldKind::Client);
        if (index == plan_.worldCount)
            return Outcome::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        const auto matches = [session, authority](const std::unique_ptr<World> &world) {
            return world && world->binding.worldSession == session && world->binding.remoteEpoch == authority;
        };
        if (matches(current_[index]))
            return Outcome::Success(plan_.worlds[index].scene);
        if (matches(candidate_[index]) && (!travelPending_ || remoteCommit_))
            return Outcome::Success(travelPending_ ? travelWorlds_[index].scene : plan_.worlds[index].scene);
        return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
    }

    /** @copydoc NetworkModeWorldState::InitialReceiver */
    ReplicationWorldLifecycle *NetworkModeWorldState::InitialReceiver(NetworkSessionGeneration session,
                                                                      std::size_t minimumObjects) noexcept {
        const auto index = Index(NetworkModeWorldKind::Client);
        const auto *binding = ClientBinding();
        if (!binding || binding->worldSession != session || !current_[index]->prefabInbox.empty())
            return nullptr;
        auto &lifecycle = current_[index]->lifecycle;
        return lifecycle.CommittedObjectCount() >= minimumObjects ? &lifecycle : nullptr;
    }

    /** @copydoc NetworkModeWorldState::PublishInitialOwnership */
    bool NetworkModeWorldState::PublishInitialOwnership(NetworkSessionGeneration session,
                                                        std::vector<ReplicationInitialOwnership> ownership,
                                                        NetworkInitialStateTarget updates) noexcept {
        const auto *binding = ClientBinding();
        if (!binding || binding->worldSession != session)
            return false;
        auto &world = *current_[Index(NetworkModeWorldKind::Client)];
        world.initialOwnership.emplace(std::move(ownership));
        world.stateUpdates = std::move(updates);
        return true;
    }

    /** @copydoc NetworkModeWorldState::ClientStateUpdates */
    const NetworkInitialStateTarget *NetworkModeWorldState::ClientStateUpdates(NetworkSessionGeneration session) const noexcept {
        const auto *binding = ClientBinding();
        if (!binding || binding->worldSession != session)
            return nullptr;
        const auto &target = current_[Index(NetworkModeWorldKind::Client)]->stateUpdates;
        return target.receiver ? &target : nullptr;
    }

    /** @copydoc NetworkModeWorldState::InitialOwnership */
    Result<ReplicationInitialOwnership> NetworkModeWorldState::InitialOwnership(Runtime::SceneRuntimeId scene,
                                                                                NetworkSessionGeneration session,
                                                                                NetworkObjectId object) const {
        using Outcome = Result<ReplicationInitialOwnership>;
        if (auto live = ResolveClientObject(scene, session, object); live.HasError())
            return Outcome::Failure(live.ErrorValue());
        const auto &published = current_[Index(NetworkModeWorldKind::Client)]->initialOwnership;
        if (!published)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationInitialStatePending));
        const auto row = std::ranges::lower_bound(*published, object, {}, &ReplicationInitialOwnership::object);
        if (row == published->end() || row->object != object)
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        return Outcome::Success(*row);
    }

    /** @copydoc NetworkModeWorldState::ClientBinding */
    const NetworkWorldBindingRecord *NetworkModeWorldState::ClientBinding() const noexcept {
        const auto kind = NetworkModeWorldKind::Client;
        const auto index = Index(kind);
        if (index == plan_.worldCount || !Ready(kind, plan_.worlds[index].scene))
            return nullptr;
        return &current_[index]->binding;
    }

    /** @copydoc NetworkModeWorldState::AcquireInitialAuthority */
    Result<AuthorityInitialView> NetworkModeWorldState::AcquireInitialAuthority(std::uint64_t tick) const {
        const auto kind = NetworkModeWorldKind::AuthorityServer;
        const auto index = Index(kind);
        const auto *binding = ServerBinding();
        if (!binding || index == plan_.worldCount || !current_[index])
            return Result<AuthorityInitialView>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        auto lease = Acquire(kind, {plan_.worlds[index].scene, binding->worldSession, Runtime::RuntimePhase::NetworkFlush, tick},
                             ReplicationWorldCapability::PublishAuthority);
        if (lease.HasError())
            return Result<AuthorityInitialView>::Failure(lease.ErrorValue());
        return Result<AuthorityInitialView>::Success({std::move(lease).Value(), current_[index]->lifecycle});
    }
}  // namespace Horo::Network::Detail
