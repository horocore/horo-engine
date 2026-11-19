#include "Horo/Network/NetworkErrors.h"
#include "NetworkModeWorldState.h"

#include <limits>
#include <new>

namespace Horo::Network::Detail {
    /** @copydoc NetworkModeWorldState::PrepareClientWorld */
    Result<std::unique_ptr<NetworkModeWorldState::World>> NetworkModeWorldState::PrepareClientWorld(Runtime::SceneRuntimeId scene,
                                                                                                    const NetworkWorldBindingRecord &record,
                                                                                                    std::uint64_t nowTick) const {
        using Outcome = Result<std::unique_ptr<World>>;
        if (!policy_.authoredScene)
            return Stage(scene, ReplicationExecutionRole::AutonomousClient, record);
        if (nowTick > std::numeric_limits<std::uint64_t>::max() - maximumPendingTicks_)
            return Outcome::Failure(MakeError(NetworkErrors::MessageCounterExhausted));
        auto lifecycle = ReplicationWorldLifecycle::Create();
        if (lifecycle.HasError())
            return Outcome::Failure(lifecycle.ErrorValue());
        try {
            auto world = std::make_unique<World>(std::move(lifecycle).Value(), record);
            world->authoredAssembly.emplace(record.worldSession, record.remoteEpoch);
            world->authoredDeadline = nowTick + maximumPendingTicks_;
            return Outcome::Success(std::move(world));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
    }

    /** @copydoc NetworkModeWorldState::AcceptAuthored */
    Result<void> NetworkModeWorldState::AcceptAuthored(std::span<const std::byte> payload, const InboundMessageContext &context) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (auto admitted = AuthorizeClient(context); admitted.HasError())
            return admitted;
        const auto index = Index(NetworkModeWorldKind::Client);
        if (index == plan_.worldCount || !candidate_[index] || !candidate_[index]->authoredAssembly)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        auto &world = *candidate_[index];
        if (context.ownerTick >= world.authoredDeadline || world.authoredDelivery.IsCancellationRequested())
            return Result<void>::Failure(MakeError(NetworkErrors::SessionTimedOut));
        auto page = DecodeNetworkAuthoredScenePage(payload);
        if (page.HasError())
            return Result<void>::Failure(page.ErrorValue());
        return AppendAuthored(world, index, page.Value(), context.cancellation);
    }

    /** @copydoc NetworkModeWorldState::AppendAuthored */
    Result<void> NetworkModeWorldState::AppendAuthored(World &world, std::size_t index, const NetworkAuthoredScenePage &page,
                                                       const CancellationToken &cancellation) {
        if (auto appended = world.authoredAssembly->Append(page); appended.HasError())
            return appended;
        world.authoredDelivery = cancellation;
        if (!world.authoredAssembly->IsComplete())
            return Result<void>::Success();
        const auto inventory = world.authoredAssembly->Take();
        if (inventory.HasError())
            return Result<void>::Failure(inventory.ErrorValue());
        const auto &local = travelPending_ ? travelClientAuthored_ : clientAuthored_;
        const auto scene = travelPending_ ? travelWorlds_[index].scene : plan_.worlds[index].scene;
        auto staged =
            world.lifecycle.StageReplicaAuthoredScene({scene, page.session, page.authority, ReplicationExecutionRole::AutonomousClient},
                                                      local.objects, inventory.Value(), local.cancellation);
        if (staged.HasValue())
            world.authoredAssembly.reset();
        else if (travelPending_)
            CancelTravel(&NetworkErrors::ReplicationDescriptorIncompatible);
        return staged;
    }

    /** @copydoc NetworkModeWorldState::AuthorityAuthored */
    const ReplicationAuthoredSceneInventory *NetworkModeWorldState::AuthorityAuthored(NetworkSessionGeneration session) const noexcept {
        const auto index = Index(NetworkModeWorldKind::AuthorityServer);
        if (stopped_ || index == plan_.worldCount)
            return nullptr;
        const auto find = [session](const std::unique_ptr<World> &world) {
            return world && world->authored && world->binding.worldSession == session ? &*world->authored : nullptr;
        };
        if (const auto *published = find(current_[index]))
            return published;
        return travelPending_ ? find(candidate_[index]) : nullptr;
    }
}  // namespace Horo::Network::Detail
