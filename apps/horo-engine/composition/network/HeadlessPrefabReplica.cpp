#include "HeadlessPrefabReplica.h"

#include "GameplayWorldComposition.h"
#include "HeadlessNetworkWorldService.h"
#include "Horo/Network/NetworkErrors.h"
#include "Horo/Network/ReplicationWorldLifecycle.h"
#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Network::NetworkErrors::NetworkObjectMappingConflict;
        using Network::NetworkErrors::NetworkObjectMappingUnknown;
        using Network::NetworkErrors::ReplicationWorldCapacityExceeded;
        constexpr std::size_t MaximumWaitingRecords = 64;
        constexpr std::uint32_t MaximumUnresolvedFrames = 120;

        /** @brief Preserves error domains when selecting a retry policy. */
        bool Matches(const Error &error, const ErrorCodeDescriptor &expected) {
            return error.domain.Value() == expected.domain.Value() && error.code.Value() == expected.code.Value();
        }

        /** @brief Requires every member schema to belong to the client's selected native generation. */
        bool HasAdmittedSchemas(const GameplayWorldComposition &gameplay, const Network::NetworkPrefabSpawnRecord &spawn) {
            const auto *installed = gameplay.Replication();
            if (!installed)
                return false;
            const auto allowed = gameplay.AdmittedReplicationSchemas();
            return std::ranges::all_of(spawn.members, [&](const Network::NetworkPrefabMemberRecord &member) {
                const auto schema = installed->Descriptors()->Find(member.schema);
                return schema.HasValue() && schema.Value()->version == member.version &&
                       std::ranges::find(allowed, member.schema) != allowed.end();
            });
        }
    }  // namespace

    /** @copydoc HeadlessPrefabReplica::Advance */
    Result<void> HeadlessPrefabReplica::Advance(GameplayWorldComposition &gameplay, const AssetCookTargetId &target,
                                                const Prefab::PrefabLimitProfile &limits, const Runtime::RuntimeSceneView &scene,
                                                const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested()) {
            Cancel();
            return Result<void>::Success();
        }
        try {
            auto received = inbox_.Take(Runtime::RuntimePhase::CommitDeferredLifecycleChanges);
            if (received.HasError()) {
                // Initial binding and travel publish after this Scene phase.
                if (Matches(received.ErrorValue(), Network::NetworkErrors::NetworkModeUnavailable) && waiting_.empty() && !active_ &&
                    instances_.empty())
                    return Result<void>::Success();
                return Result<void>::Failure(received.ErrorValue());
            }
            if (auto queued = Enqueue(std::move(received).Value()); queued.HasError())
                return queued;
            if (auto observed = Observe(); observed.HasError())
                return observed;
            if (active_ || waiting_.empty())
                return Result<void>::Success();
            return Schedule(gameplay, target, limits, scene, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(ReplicationWorldCapacityExceeded));
        }
    }

    /** @copydoc HeadlessPrefabReplica::Cancel */
    void HeadlessPrefabReplica::Cancel() noexcept {
        if (active_)
            active_->receipt.Cancel();
        active_.reset();
        waiting_.clear();
        instances_.clear();
        unresolvedFrames_ = 0;
    }

    /** @brief Rejects duplicate spawn identities before any structural work is queued. */
    Result<void> HeadlessPrefabReplica::Enqueue(std::vector<Network::NetworkPrefabLifecycleRecord> records) {
        if (records.size() > MaximumWaitingRecords - waiting_.size())
            return Result<void>::Failure(MakeError(ReplicationWorldCapacityExceeded));
        for (auto &record : records) {
            if (record.spawn) {
                const auto duplicate = [&record](const Network::NetworkPrefabLifecycleRecord &queued) {
                    return queued.spawn && queued.occurrence == record.occurrence;
                };
                if (instances_.contains(record.occurrence) || std::ranges::any_of(waiting_, duplicate))
                    return Result<void>::Failure(MakeError(NetworkObjectMappingConflict));
            }
            waiting_.push_back(std::move(record));
        }
        return Result<void>::Success();
    }

    /** @brief Transfers an instance handle only after the matching Scene transaction commits. */
    Result<void> HeadlessPrefabReplica::Observe() {
        if (!active_ || active_->receipt.State() == Prefab::PrefabSpawnState::Pending)
            return Result<void>::Success();
        auto operation = std::exchange(active_, std::nullopt).value();
        if (const auto *error = operation.receipt.Failure())
            return Result<void>::Failure(*error);
        if (operation.receipt.State() == Prefab::PrefabSpawnState::Cancelled)
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
        if (operation.creating) {
            auto instance = operation.receipt.Spawned();
            if (instance.HasError())
                return Result<void>::Failure(instance.ErrorValue());
            instances_.find(operation.occurrence)->second = std::move(instance).Value();
        } else {
            instances_.erase(operation.occurrence);
        }
        return Result<void>::Success();
    }

    /** @brief Lets a dependency spawn pass an unresolved spawn while preserving retirement order. */
    Result<void> HeadlessPrefabReplica::Schedule(GameplayWorldComposition &gameplay, const AssetCookTargetId &target,
                                                 const Prefab::PrefabLimitProfile &limits, const Runtime::RuntimeSceneView &scene,
                                                 const CancellationToken &cancellation) {
        for (auto item = waiting_.begin(); item != waiting_.end(); ++item) {
            if (!item->spawn) {
                if (item != waiting_.begin())
                    break;
                auto retired = Despawn(gameplay, item->occurrence, cancellation);
                if (retired.HasError())
                    return retired;
            } else {
                auto request = DecodeHeadlessPrefabIntent(*item, target, limits, scene, inbox_);
                if (request.HasError()) {
                    if (Matches(request.ErrorValue(), NetworkObjectMappingUnknown))
                        continue;
                    return Result<void>::Failure(request.ErrorValue());
                }
                auto spawned = Spawn(gameplay, *item, std::move(request).Value(), cancellation);
                if (spawned.HasError())
                    return spawned;
            }
            waiting_.erase(item);
            unresolvedFrames_ = 0;
            return Result<void>::Success();
        }
        if (++unresolvedFrames_ == MaximumUnresolvedFrames)
            return Result<void>::Failure(MakeError(NetworkObjectMappingUnknown, "Unresolved replicated Prefab relationships."));
        return Result<void>::Success();
    }

    /** @brief Reserves retention before queuing an atomic spawn. */
    Result<void> HeadlessPrefabReplica::Spawn(GameplayWorldComposition &gameplay, const Network::NetworkPrefabLifecycleRecord &record,
                                              Prefab::PrefabSpawnRequest request, const CancellationToken &cancellation) {
        if (instances_.size() == Network::ReplicationWorldLimits{}.maximumObjects)
            return Result<void>::Failure(MakeError(ReplicationWorldCapacityExceeded));
        if (!HasAdmittedSchemas(gameplay, *record.spawn))
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationDescriptorIncompatible));
        auto publication = inbox_.Bind(record, cancellation);
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        const auto [position, inserted] = instances_.try_emplace(record.occurrence);
        if (!inserted)
            return Result<void>::Failure(MakeError(NetworkObjectMappingConflict));
        auto operation = gameplay.SpawnReplicaPrefab(std::move(request), std::move(publication).Value(), cancellation);
        if (operation.HasError()) {
            instances_.erase(position);
            return Result<void>::Failure(operation.ErrorValue());
        }
        active_.emplace(record.occurrence, std::move(operation).Value(), true);
        return Result<void>::Success();
    }

    /** @brief Retires only the exact generation's committed handle. */
    Result<void> HeadlessPrefabReplica::Despawn(GameplayWorldComposition &gameplay, Network::NetworkObjectId occurrence,
                                                const CancellationToken &cancellation) {
        const auto found = instances_.find(occurrence);
        if (found == instances_.end())
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldStale));
        auto operation = gameplay.DespawnReplicaPrefab(found->second, cancellation);
        if (operation.HasError())
            return Result<void>::Failure(operation.ErrorValue());
        active_.emplace(occurrence, std::move(operation).Value(), false);
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNetworkDetail::WorldService::BindReplicaPrefabs */
    Result<void> HeadlessNetworkDetail::WorldService::BindReplicaPrefabs() {
        if (!request_.prefabInbox.IsBound())
            return Result<void>::Success();
        if (!record_->templates || !owners_->configured->prefabTarget.IsValid())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::AdmissionRejected,
                                                   "Client Prefab replication requires an admitted local cook target and loader."));
        record_->prefabReplica.emplace(request_.prefabInbox);
        return Result<void>::Success();
    }

    /** @copydoc HeadlessNetworkDetail::WorldService::AdvanceReplicaPrefabs */
    Result<void> HeadlessNetworkDetail::WorldService::AdvanceReplicaPrefabs() {
        const auto scene = ActiveScene();
        if (!scene || !record_->activeGameplay || !record_->templates)
            return Result<void>::Failure(MakeError(Network::NetworkErrors::ReplicationWorldUnavailable));
        return record_->prefabReplica->Advance(*record_->activeGameplay, owners_->configured->prefabTarget,
                                               record_->templates->LimitProfile(), *scene, record_->sceneCancellation);
    }
}  // namespace Horo::Application::Internal
