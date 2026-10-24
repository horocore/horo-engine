#include "NetworkModeWorldState.h"

#include "Horo/Network/NetworkErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Network::Detail {
    NetworkModeWorldState::NetworkModeWorldState(NetworkModePlan plan, NetworkWorldBindingPolicy policy,
                                                 const std::uint64_t schemaFingerprint)
        : plan_(std::move(plan)), policy_(std::move(policy)), schemaFingerprint_(schemaFingerprint) {}

    NetworkModeWorldState::~NetworkModeWorldState() noexcept {
        Shutdown();
    }

    Result<void> NetworkModeWorldState::CheckOwner() const {
        if (std::this_thread::get_id() != owner_ || stopped_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        return Result<void>::Success();
    }

    std::size_t NetworkModeWorldState::Index(const NetworkModeWorldKind kind) const noexcept {
        for (std::size_t index = 0; index < plan_.worldCount; ++index)
            if (plan_.worlds[index].kind == kind)
                return index;
        return plan_.worldCount;
    }

    Result<std::unique_ptr<NetworkModeWorldState::World>> NetworkModeWorldState::Stage(const Runtime::SceneRuntimeId scene,
                                                                                       const ReplicationExecutionRole role,
                                                                                       const NetworkWorldBindingRecord &record) const {
        auto created = ReplicationWorldLifecycle::Create();
        if (created.HasError())
            return Result<std::unique_ptr<World>>::Failure(created.ErrorValue());
        auto lifecycle = std::move(created).Value();
        if (auto staged = lifecycle.Stage({scene, record.worldSession, record.remoteEpoch, role}); staged.HasError())
            return Result<std::unique_ptr<World>>::Failure(staged.ErrorValue());
        try {
            return Result<std::unique_ptr<World>>::Success(std::make_unique<World>(std::move(lifecycle), record));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<World>>::Failure(MakeError(NetworkErrors::ReplicationWorldCapacityExceeded));
        }
    }

    Result<void> NetworkModeWorldState::Prepare() {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        const auto server = Index(NetworkModeWorldKind::AuthorityServer);
        if (server == plan_.worldCount)
            return Result<void>::Success();
        if (current_[server] || candidate_[server])
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        auto issued = issuer_.Issue(plan_.worlds[server].scene);
        if (issued.HasError())
            return Result<void>::Failure(issued.ErrorValue());
        auto staged = Stage(issued.Value().scene, issued.Value().role,
                            {NetworkWorldBindingKind::Initial, issued.Value().session, issued.Value().authority, schemaFingerprint_, {}});
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        candidate_[server] = std::move(staged).Value();
        return Result<void>::Success();
    }

    Result<void> NetworkModeWorldState::PrepareTravel(const std::span<const NetworkModeWorld> worlds, const std::uint64_t nowTick,
                                                      const std::uint64_t maximumPendingTicks) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (travelPending_ || worlds.size() != plan_.worldCount || nowTick < nowTick_ || maximumPendingTicks == 0 ||
            nowTick > std::numeric_limits<std::uint64_t>::max() - maximumPendingTicks)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        for (std::size_t index = 0; index < worlds.size(); ++index) {
            if (!current_[index] || candidate_[index] || worlds[index].kind != plan_.worlds[index].kind || !worlds[index].scene.IsValid() ||
                worlds[index].scene == plan_.worlds[index].scene || (index != 0 && worlds[0].scene == worlds[index].scene))
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        }
        if (const auto server = Index(NetworkModeWorldKind::AuthorityServer); server != plan_.worldCount) {
            auto issued = issuer_.Issue(worlds[server].scene);
            if (issued.HasError())
                return Result<void>::Failure(issued.ErrorValue());
            const auto prior = NetworkWorldBindingDigest(current_[server]->binding);
            if (prior.HasError())
                return Result<void>::Failure(prior.ErrorValue());
            auto staged = Stage(worlds[server].scene, ReplicationExecutionRole::AuthorityServer,
                                {NetworkWorldBindingKind::PrepareTravel, issued.Value().session, issued.Value().authority,
                                 schemaFingerprint_, prior.Value()});
            if (staged.HasError())
                return Result<void>::Failure(staged.ErrorValue());
            candidate_[server] = std::move(staged).Value();
        }
        std::ranges::copy(worlds, travelWorlds_.begin());
        travelDeadline_ = nowTick + maximumPendingTicks;
        travelFailure_ = nullptr;
        travelPending_ = true;
        remoteCommit_ = false;
        return Result<void>::Success();
    }

    bool NetworkModeWorldState::TravelReady() const noexcept {
        if (!travelPending_ || (plan_.mode == NetworkProjectRole::Client && !remoteCommit_))
            return false;
        for (std::size_t index = 0; index < plan_.worldCount; ++index)
            if (!candidate_[index])
                return false;
        return true;
    }

    const NetworkWorldBindingRecord *NetworkModeWorldState::PreparedServerBinding() const noexcept {
        const auto server = Index(NetworkModeWorldKind::AuthorityServer);
        return !stopped_ && travelPending_ && server != plan_.worldCount && candidate_[server] ? &candidate_[server]->binding : nullptr;
    }

    void NetworkModeWorldState::CancelTravel(const ErrorCodeDescriptor *reason) noexcept {
        if (!travelPending_)
            return;
        if (!travelFailure_)
            travelFailure_ = reason ? reason : &NetworkErrors::SessionCancelled;
        for (auto &candidate : candidate_) {
            if (candidate)
                candidate->lifecycle.BeginShutdown();
            candidate.reset();
        }
        travelPending_ = false;
        remoteCommit_ = false;
        travelDeadline_ = 0;
    }

    /** @copydoc NetworkModeWorldState::TravelStatus */
    Result<void> NetworkModeWorldState::TravelStatus() const {
        if (travelPending_)
            return Result<void>::Success();
        return Result<void>::Failure(MakeError(travelFailure_ ? *travelFailure_ : NetworkErrors::NetworkModeStale));
    }

    Result<void> NetworkModeWorldState::BindClient(std::shared_ptr<PeerSessionLifecycle> peer, const ConnectionHandle connection,
                                                   const NetworkOperationGeneration generation, const std::uint64_t nowTick) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        Reconcile(nowTick);
        if (!peer || Index(NetworkModeWorldKind::Client) == plan_.worldCount || nowTick < nowTick_ ||
            (client_ && client_ != peer && (connection_ == connection || generation <= peerGeneration_)))
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (auto active = peer->AdmitGameplay(connection, generation, nowTick); active.HasError())
            return active;
        if (const auto *authentication = peer->Authentication();
            !authentication || authentication->principal.principal != policy_.expectedServer ||
            authentication->principal.provenance != policy_.expectedProvenance ||
            std::ranges::find(authentication->principal.capabilities.Values(), policy_.authorityPublisher) ==
                authentication->principal.capabilities.Values().end())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        if (client_ && client_ != peer) {
            const auto index = Index(NetworkModeWorldKind::Client);
            if (current_[index])
                current_[index]->lifecycle.BeginShutdown();
            current_[index].reset();
            candidate_[index].reset();
            CancelTravel();
        }
        client_ = std::move(peer);
        connection_ = connection;
        peerGeneration_ = generation;
        nowTick_ = nowTick;
        return Result<void>::Success();
    }

    Result<void> NetworkModeWorldState::UnbindClient(const NetworkOperationGeneration generation) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (generation != peerGeneration_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (!client_)
            return Result<void>::Success();  // The same terminal was reconciled before the host projection.
        const auto index = Index(NetworkModeWorldKind::Client);
        if (current_[index])
            current_[index]->lifecycle.BeginShutdown();
        current_[index].reset();
        candidate_[index].reset();
        const auto *terminal = client_->Terminal();
        CancelTravel(terminal && terminal->failure ? &terminal->failure->Descriptor() : &NetworkErrors::NetworkModeStale);
        client_.reset();
        return Result<void>::Success();
    }

    Result<void> NetworkModeWorldState::AuthorizeClient(const InboundMessageContext &context) const {
        if (context.session != client_ || context.connection != connection_ || context.generation != peerGeneration_ ||
            context.ownerTick < nowTick_ || context.cancellation.IsCancellationRequested() ||
            context.delivery != DeliveryPolicy::ReliableOrdered)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (!client_)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (auto active = client_->AdmitGameplay(connection_, peerGeneration_, context.ownerTick); active.HasError())
            return active;
        const auto *authentication = client_->Authentication();
        if (const auto *negotiation = client_->Negotiation();
            !authentication || !negotiation || authentication->principal.principal != policy_.expectedServer ||
            authentication->principal.provenance != policy_.expectedProvenance ||
            std::ranges::find(authentication->principal.capabilities.Values(), policy_.authorityPublisher) ==
                authentication->principal.capabilities.Values().end() ||
            negotiation->protocol != policy_.control.protocol || negotiation->version != plan_.protocolVersion ||
            negotiation->schemaFingerprint != schemaFingerprint_ ||
            std::ranges::find(negotiation->features.Values(), policy_.feature) == negotiation->features.Values().end())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        return Result<void>::Success();
    }

    Result<void> NetworkModeWorldState::Accept(const NetworkWorldBindingRecord &record, const InboundMessageContext &context) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (auto authorized = AuthorizeClient(context); authorized.HasError())
            return authorized;
        if (auto canonical = EncodeNetworkWorldBinding(record); canonical.HasError())
            return Result<void>::Failure(canonical.ErrorValue());
        if (record.schemaFingerprint != schemaFingerprint_)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationDescriptorIncompatible));
        const auto index = Index(NetworkModeWorldKind::Client);
        if (index == plan_.worldCount)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeInvalid));
        if (current_[index] && current_[index]->binding == record)
            return Result<void>::Success();
        if (record.kind != NetworkWorldBindingKind::Initial)
            return AcceptTravel(record, context, index);
        if (current_[index] || candidate_[index])
            return candidate_[index] && candidate_[index]->binding == record
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        return StageClient(record, index, context.ownerTick);
    }

    /** @copydoc NetworkModeWorldState::AcceptTravel */
    Result<void> NetworkModeWorldState::AcceptTravel(const NetworkWorldBindingRecord &record, const InboundMessageContext &context,
                                                     const std::size_t index) {
        if (!travelPending_ || context.ownerTick >= travelDeadline_ || !current_[index])
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        const auto prior = NetworkWorldBindingDigest(current_[index]->binding);
        if (prior.HasError())
            return Result<void>::Failure(prior.ErrorValue());
        if (record.expectedPriorBinding != prior.Value() || record.worldSession <= current_[index]->binding.worldSession ||
            record.remoteEpoch <= current_[index]->binding.remoteEpoch)
            return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        auto prepared = record;
        prepared.kind = NetworkWorldBindingKind::PrepareTravel;
        if (record.kind == NetworkWorldBindingKind::CommitTravel) {
            if (!candidate_[index] || candidate_[index]->binding != prepared)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
            remoteCommit_ = true;
            return Result<void>::Success();
        }
        if (candidate_[index])
            return candidate_[index]->binding == prepared ? Result<void>::Success()
                                                          : Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        return StageClient(record, index, context.ownerTick);
    }

    /** @copydoc NetworkModeWorldState::StageClient */
    Result<void> NetworkModeWorldState::StageClient(const NetworkWorldBindingRecord &record, const std::size_t index,
                                                    const std::uint64_t nowTick) {
        // A listen client can only mirror its actual local server occurrence,
        // while still requiring the real authenticated inbound control path.
        if (const auto server = Index(NetworkModeWorldKind::AuthorityServer); server != plan_.worldCount) {
            const auto *expected = travelPending_ ? PreparedServerBinding() : ServerBinding();
            auto selected = expected ? *expected : NetworkWorldBindingRecord{};
            if (record.kind == NetworkWorldBindingKind::Initial) {
                selected.kind = NetworkWorldBindingKind::Initial;
                selected.expectedPriorBinding = {};
            }
            if (!expected || selected != record)
                return Result<void>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        }
        const auto scene = travelPending_ ? travelWorlds_[index].scene : plan_.worlds[index].scene;
        auto staged = Stage(scene, ReplicationExecutionRole::AutonomousClient, record);
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        candidate_[index] = std::move(staged).Value();
        nowTick_ = nowTick;
        return Result<void>::Success();
    }

    Result<void> NetworkModeWorldState::Commit(const Runtime::RuntimePhase phase) {
        if (auto owner = CheckOwner(); owner.HasError())
            return owner;
        if (phase != Runtime::RuntimePhase::CommitDeferredLifecycleChanges)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldPhaseInvalid));
        if (travelPending_ && !TravelReady())
            return Result<void>::Success();
        // These lifecycles are detached: every fallible commit completes before any role can see a replacement.
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (!candidate_[index])
                continue;
            if (candidate_[index]->lifecycle.State() == ReplicationWorldLifecycleState::Active)
                continue;
            const auto &record = candidate_[index]->binding;
            if (auto committed =
                    candidate_[index]->lifecycle.CommitAtSafePoint(travelPending_ ? travelWorlds_[index].scene : plan_.worlds[index].scene,
                                                                   record.worldSession, phase);
                committed.HasError())
                return committed;
        }
        for (std::size_t index = 0; index < plan_.worldCount; ++index) {
            if (!candidate_[index])
                continue;
            if (current_[index])
                current_[index]->lifecycle.BeginShutdown();
            current_[index] = std::move(candidate_[index]);
        }
        bool complete = true;
        for (std::size_t index = 0; index < plan_.worldCount; ++index)
            complete = complete && static_cast<bool>(current_[index]);
        everPublished_ = everPublished_ || complete;
        if (travelPending_) {
            plan_.worlds = travelWorlds_;
            for (std::size_t index = 0; index < plan_.worldCount; ++index)
                current_[index]->binding.kind = NetworkWorldBindingKind::CommitTravel;
            travelPending_ = false;
            remoteCommit_ = false;
        }
        return Result<void>::Success();
    }

    void NetworkModeWorldState::Reconcile(const std::uint64_t nowTick) noexcept {
        if (travelPending_ && (nowTick < nowTick_ || nowTick >= travelDeadline_))
            CancelTravel(nowTick < nowTick_ ? &NetworkErrors::NetworkModeStale : &NetworkErrors::SessionTimedOut);
        if (!client_)
            return;
        if (nowTick >= nowTick_ && client_->State() == PeerSessionState::Active) {
            nowTick_ = nowTick;
            return;
        }
        if (const auto index = Index(NetworkModeWorldKind::Client); index != plan_.worldCount) {
            if (current_[index])
                current_[index]->lifecycle.BeginShutdown();
            current_[index].reset();
            candidate_[index].reset();
        }
        const auto *terminal = client_->Terminal();
        CancelTravel(terminal && terminal->failure ? &terminal->failure->Descriptor() : &NetworkErrors::SessionCancelled);
        client_.reset();
    }

    bool NetworkModeWorldState::Ready(const NetworkModeWorldKind kind, const Runtime::SceneRuntimeId scene) const noexcept {
        const auto index = Index(kind);
        return !stopped_ && std::this_thread::get_id() == owner_ && index != plan_.worldCount && plan_.worlds[index].scene == scene &&
               current_[index] && current_[index]->lifecycle.State() == ReplicationWorldLifecycleState::Active &&
               (plan_.mode != NetworkProjectRole::ListenServer || everPublished_) &&
               (kind != NetworkModeWorldKind::Client || (client_ && client_->State() == PeerSessionState::Active));
    }

    /** @copydoc NetworkModeWorldState::ReadyForPeer */
    bool NetworkModeWorldState::ReadyForPeer(const bool inboundServer) const noexcept {
        const auto kind = inboundServer ? NetworkModeWorldKind::AuthorityServer : NetworkModeWorldKind::Client;
        for (std::size_t index = 0; index < plan_.worldCount; ++index)
            if (plan_.worlds[index].kind == kind)
                return Ready(kind, plan_.worlds[index].scene);
        return false;
    }

    Result<NetworkModeRoleView> NetworkModeWorldState::Role(const NetworkModeWorldKind kind, const Runtime::SceneRuntimeId scene) const {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<NetworkModeRoleView>::Failure(owner.ErrorValue());
        const auto index = Index(kind);
        if (index == plan_.worldCount || plan_.worlds[index].scene != scene)
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeStale));
        if (!Ready(kind, scene))
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (plan_.mode == NetworkProjectRole::ListenServer && !everPublished_)
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        const auto active = current_[index]->lifecycle.ActiveDescriptor();
        if (active.HasError())
            return Result<NetworkModeRoleView>::Failure(active.ErrorValue());
        if (kind == NetworkModeWorldKind::Client && (!client_ || client_->State() != PeerSessionState::Active))
            return Result<NetworkModeRoleView>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        const auto &descriptor = active.Value();
        return Result<NetworkModeRoleView>::Success(
            {plan_.hostGeneration, descriptor.scene, descriptor.role,
             kind == NetworkModeWorldKind::Client ? std::optional{peerGeneration_} : std::nullopt,
             kind == NetworkModeWorldKind::AuthorityServer ? descriptor.authority : ReplicationAuthorityEpoch{}, descriptor.session});
    }

    Result<ReplicationWorldReadLease> NetworkModeWorldState::Acquire(const NetworkModeWorldKind kind,
                                                                     const ReplicationWorldWorkRequest &request,
                                                                     const ReplicationWorldCapability capability) const {
        if (auto owner = CheckOwner(); owner.HasError())
            return Result<ReplicationWorldReadLease>::Failure(owner.ErrorValue());
        const auto index = Index(kind);
        if (index == plan_.worldCount || !current_[index])
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::ReplicationWorldUnavailable));
        if (plan_.mode == NetworkProjectRole::ListenServer && !everPublished_)
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
        if (kind == NetworkModeWorldKind::Client) {
            if (!client_)
                return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::NetworkModeUnavailable));
            if (auto active = client_->AdmitGameplay(connection_, peerGeneration_, nowTick_); active.HasError())
                return Result<ReplicationWorldReadLease>::Failure(active.ErrorValue());
        }
        return current_[index]->lifecycle.AcquireFor(request, capability);
    }

    const NetworkWorldBindingRecord *NetworkModeWorldState::ServerBinding() const noexcept {
        const auto index = Index(NetworkModeWorldKind::AuthorityServer);
        return !stopped_ && index != plan_.worldCount && current_[index] ? &current_[index]->binding : nullptr;
    }

    void NetworkModeWorldState::Shutdown() noexcept {
        if (stopped_)
            return;
        stopped_ = true;
        for (const auto &world : current_)
            if (world)
                world->lifecycle.BeginShutdown();
        for (const auto &world : candidate_)
            if (world)
                world->lifecycle.BeginShutdown();
        client_.reset();
    }
}  // namespace Horo::Network::Detail
