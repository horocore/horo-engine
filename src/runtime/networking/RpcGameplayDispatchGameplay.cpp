#include "Horo/Network/NetworkErrors.h"
#include "RpcGameplayDispatchState.h"

#include <algorithm>
#include <new>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Horo::Network {
    using Detail::DispatchFlagGuard;

    /** @copydoc RpcGameplayDispatch::ValidateTarget */
    Result<void> RpcGameplayDispatch::ValidateTarget(const Peer &peer, const RpcDescriptor &descriptor, const ReplicationRoleBinding &role,
                                                     const NetworkPeerId wireRecipient) const {
        if (peer.role == RpcRemoteRole::Client) {
            if (descriptor.direction != RpcDirection::ClientToAuthority || descriptor.target != RpcTarget::Authority ||
                role.role != ReplicationExecutionRole::AuthorityServer || wireRecipient.IsValid())
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationAuthorityDenied));
        } else {
            using enum RpcTarget;
            if (descriptor.direction != RpcDirection::AuthorityToClient || role.role == ReplicationExecutionRole::AuthorityServer ||
                !wireRecipient.IsValid() || wireRecipient != peer.localRecipient || role.localPeer != peer.localRecipient)
                return Result<void>::Failure(MakeError(NetworkErrors::ReplicationAuthorityDenied));
            switch (descriptor.target) {
                case InvokingClient:
                case AllClients:
                    break;
                case ObjectOwner:
                    if (!role.autonomousOwner || *role.autonomousOwner != peer.localRecipient)
                        return Result<void>::Failure(MakeError(NetworkErrors::ReplicationAuthorityDenied));
                    break;
                case Authority:
                case Count:
                    return Result<void>::Failure(MakeError(NetworkErrors::ReplicationAuthorityDenied));
            }
        }
        using enum RpcCallerPermission;
        switch (descriptor.permission) {
            case AuthenticatedPeer:
                return Result<void>::Success();
            case ObjectOwner:
                if (role.autonomousOwner && *role.autonomousOwner == peer.identity)
                    return Result<void>::Success();
                break;
            case AuthorityOnly:
                if (peer.role == RpcRemoteRole::Authority)
                    return Result<void>::Success();
                break;
            case Custom:
                return Result<void>::Failure(MakeError(NetworkErrors::RpcPermissionUnsupported));
            case Count:
                break;
        }
        return Result<void>::Failure(MakeError(NetworkErrors::ReplicationAuthorityDenied));
    }

    /** @copydoc RpcGameplayDispatch::ValidateLive */
    Result<ReplicationWorldReadLease> RpcGameplayDispatch::ValidateLive(const Peer &peer, const Object &object,
                                                                        const RpcDescriptor &descriptor, const NetworkPeerId wireRecipient,
                                                                        const std::uint64_t nowTick, const Runtime::RuntimePhase phase,
                                                                        Runtime::EntityRef &entity) const {
        const auto &target = object.identity;
        if (const auto admitted = peer.session->AdmitGameplay(peer.connection, peer.generation, nowTick); admitted.HasError())
            return Result<ReplicationWorldReadLease>::Failure(admitted.ErrorValue());
        const auto active = world_.ActiveDescriptor();
        if (active.HasError())
            return Result<ReplicationWorldReadLease>::Failure(active.ErrorValue());
        auto lease = world_.Acquire({active.Value().scene, active.Value().session, phase, nowTick, {}});
        if (lease.HasError())
            return Result<ReplicationWorldReadLease>::Failure(lease.ErrorValue());
        if (lease.Value().Descriptor().role == ReplicationExecutionRole::Standalone ||
            lease.Value().Descriptor().authority != target.Epoch())
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        const auto role = object.role->Snapshot();
        if (role.HasError() || role.Value().session != lease.Value().Descriptor().session || role.Value().object != target ||
            role.Value().role != lease.Value().Descriptor().role)
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        const auto entries = lease.Value().Mapping().Entries();
        const auto mapped = std::ranges::find_if(entries, [target](const NetworkObjectMappingEntry &entry) {
            return entry.object == target;
        });
        if (mapped == entries.end() || role.Value().schema != mapped->provenance.schema ||
            role.Value().schemaVersion != mapped->provenance.schemaVersion)
            return Result<ReplicationWorldReadLease>::Failure(MakeError(NetworkErrors::NetworkObjectMappingUnknown));
        if (const auto allowed = ValidateTarget(peer, descriptor, role.Value(), wireRecipient); allowed.HasError())
            return Result<ReplicationWorldReadLease>::Failure(allowed.ErrorValue());
        entity = mapped->entity;
        return Result<ReplicationWorldReadLease>::Success(std::move(lease).Value());
    }

    /** @copydoc RpcGameplayDispatch::DrainAtGameplaySafePoint */
    Result<RpcDispatchReport> RpcGameplayDispatch::DrainAtGameplaySafePoint(const ReplicationWorldWorkRequest &request) {
        if (const auto owner = CheckOwner(); owner.HasError())
            return Result<RpcDispatchReport>::Failure(owner.ErrorValue());
        if (request.phase != Runtime::RuntimePhase::FixedUpdate || request.simulationTick == 0)
            return Result<RpcDispatchReport>::Failure(MakeError(NetworkErrors::ReplicationWorldPhaseInvalid));
        if (request.cancellation.IsCancellationRequested())
            return Result<RpcDispatchReport>::Failure(MakeError(NetworkErrors::ReplicationWorldCancelled));
        const auto active = world_.ActiveDescriptor();
        if (active.HasError())
            return Result<RpcDispatchReport>::Failure(active.ErrorValue());
        if (request.scene != active.Value().scene || request.session != active.Value().session)
            return Result<RpcDispatchReport>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        if (draining_)
            return Result<RpcDispatchReport>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        [[maybe_unused]] const auto self = shared_from_this();
        const DispatchFlagGuard guard{draining_};
        RpcDispatchReport report;
        const auto count = std::min(pending_.size(), limits_.maximumPerDrain);
        for (std::size_t index = 0; index < count; ++index) {
            if (stopped_ || pending_.empty() || request.cancellation.IsCancellationRequested())
                break;
            Pending command = std::move(pending_.front());
            pending_.erase(pending_.begin());
            ++report.consumed;
            if (const auto executed = ExecutePending(command, request); executed.HasError()) {
                ++report.rejected;
                report.lastError = executed.ErrorValue();
            } else {
                ++report.invoked;
            }
        }
        return Result<RpcDispatchReport>::Success(std::move(report));
    }

    /** @copydoc RpcGameplayDispatch::ExecutePending */
    Result<void> RpcGameplayDispatch::ExecutePending(const Pending &command, const ReplicationWorldWorkRequest &request) {
        if (command.scene != request.scene || command.session != request.session)
            return Result<void>::Failure(MakeError(NetworkErrors::ReplicationWorldStale));
        if (command.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(NetworkErrors::SessionCancelled));
        const auto peer = std::ranges::find_if(peers_, [&command](const Peer &entry) {
            return entry.connection == command.connection && entry.generation == command.generation;
        });
        const auto object = std::ranges::find_if(objects_, [&command](const Object &entry) {
            return entry.identity == command.object;
        });
        const auto binding = std::ranges::find_if(bindings_, [&command](const Binding &entry) {
            return entry.id == command.id;
        });
        if (peer == peers_.end() || object == objects_.end() || binding == bindings_.end())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        Runtime::EntityRef entity;
        const auto live = ValidateLive(*peer, *object, *binding->descriptor, command.recipient, request.simulationTick,
                                       Runtime::RuntimePhase::FixedUpdate, entity);
        [[maybe_unused]] const auto moduleLease = binding->moduleLease;
        const auto handler = binding->handler.lock();
        if (live.HasError())
            return Result<void>::Failure(live.ErrorValue());
        if (!handler || live.Value().IsRevoked() || live.Value().Cancellation().IsCancellationRequested())
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        try {
            const RpcGameplayContext context{peer->identity,   command.object,        entity, request.scene, request.session,
                                             command.sequence, request.simulationTick};
            return handler->Execute(context, command.values);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(NetworkErrors::RpcCapacityExceeded));
        } catch (const std::invalid_argument &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (const std::out_of_range &) {
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        } catch (...) {
            // Module callbacks may throw non-standard values; no exception may cross the host Gameplay boundary.
            return Result<void>::Failure(MakeError(NetworkErrors::GameplayDispatchRejected));
        }
    }

}  // namespace Horo::Network
