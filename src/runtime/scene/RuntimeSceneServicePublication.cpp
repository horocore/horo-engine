#include "ActivatedSceneInstancesInternal.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"
#include "RuntimeScenePublicationInternal.h"

#include <algorithm>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc RuntimeSceneService::BindReplicaTransformDispatch */
    Result<void> RuntimeSceneService::BindReplicaTransformDispatch(RuntimeDispatchSource source) {
        // Travel creates the next Scene on the owner lane while the same issuer dispatches. Binding is inert.
        if (const auto status = source.BindingStatus();
            status != RuntimeDispatchStatus::Valid && status != RuntimeDispatchStatus::Reentrant)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (started_ || replicaTransformDispatch_.IsValid() || mutatingLifecycle_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        replicaTransformDispatch_ = std::move(source);
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::CommitReplicaTransformBatch */
    Result<void> RuntimeSceneService::CommitReplicaTransformBatch(const RuntimeSceneView &expected, const SceneCommandBuffer &commands,
                                                                  const RuntimeDispatchEvidence &evidence) {
        RuntimeDispatchFacts facts;
        if (evidence.Read(replicaTransformDispatch_, RuntimePhase::CommitDeferredLifecycleChanges, facts) != RuntimeDispatchStatus::Valid)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (!active_.scene || !expected.SameIncarnation(active_.scene->View()) || !expected.IsCurrent())
            return Result<void>::Failure(MakeError(SceneErrors::StaleView));
        if (mutatingLifecycle_ || transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        const auto admittedTransform = [](const auto &command) {
            const auto *transform = std::get_if<SceneCommandBuffer::SetLocalTransformCommand>(&command);
            return transform && transform->admission.has_value();
        };
        if (commands.Empty() || !std::ranges::all_of(commands.commands_, admittedTransform))
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
        const LifecycleMutation mutation{*this};
        const auto committed = active_.scene->CommitWithRegistry(commands, assetPreparationServices_.registry, {}, nullptr,
                                                                 RuntimeScene::CommitPurpose::ReplicaTransforms);
        return committed.HasValue() ? Result<void>::Success() : Result<void>::Failure(committed.ErrorValue());
    }

    /** @copydoc ScenePublicationDetail::ShutdownCandidates */
    void ScenePublicationDetail::ShutdownCandidates(std::vector<std::unique_ptr<SceneActivationCandidate>> &candidates) noexcept {
        for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate)
            (*candidate)->Shutdown();
        candidates.clear();
    }

    /** @brief Private no-fail transfer retaining the retired Scene through save operation completion. */
    struct RuntimeSceneService::AggregateTransfer final : IStagedRestoreAggregatePublication {
        explicit AggregateTransfer(RuntimeSceneService &service) noexcept : service(service), retired(service.pending_) {}

        void PublishPrepared() noexcept override {
            for (const auto &candidate : service.pending_.candidates)
                candidate->Publish();
            service.pending_.restore = std::move(service.aggregateRestore_);
            // Reuse the prepared buffers: publication cannot allocate checked-library iterator proxies.
            service.active_.scene.swap(service.pending_.scene);
            service.active_.candidates.swap(service.pending_.candidates);
            std::swap(service.active_.datasets, service.pending_.datasets);
            service.active_.restore.swap(service.pending_.restore);
            retiredChildren.swap(service.activatedInstances_);
            if (service.initialActivatedChildren_)
                service.activatedInstances_.swap(service.initialActivatedChildren_->ready);
            if (service.publicationReceipt_) {
                const auto view = service.active_.scene->View();
                service.publicationReceipt_->snapshot = {ScenePublicationStatus::Published, view.RuntimeId(), view.StructuralRevision(),
                                                         service.active_.datasets};
            }
        }

        RuntimeSceneService &service;
        SceneAggregate &retired;
        std::unique_ptr<Detail::ActivatedSceneInstances> retiredChildren;
    };

    /** @copydoc RuntimeSceneService::PreparePendingPublication */
    Result<bool> RuntimeSceneService::PreparePendingPublication(const FrameContext &context) {
        if (initialActivatedChildren_ && !initialActivatedChildren_->ready) {
            auto children = Detail::ActivatedSceneInstances::PrepareInitial(*this, context);
            if (children.HasError())
                return Result<bool>::Failure(children.ErrorValue());
            initialActivatedChildren_->ready = std::move(children).Value();
        }
        if (aggregateRestore_) {
            const auto ready = aggregateRestore_->PrepareOwners();
            if (ready.HasError())
                return Result<bool>::Failure(ready.ErrorValue());
            if (!ready.Value())
                return Result<bool>::Success(false);
            const auto active = ActiveScene();
            if (const auto valid = aggregateRestore_->ValidatePublication(active ? &*active : nullptr); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
        }
        if (publicationCheck_) {
            if (const auto valid = publicationCheck_->ValidatePublication(); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (const auto valid = publicationCheck_->ValidatePreparedComposition(pending_.datasets); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
        }
        for (const auto &candidate : pending_.candidates) {
            if (const auto valid = candidate->ValidatePublication(); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (shutdownDeferred_)
                return Result<bool>::Failure(MakeError(SceneErrors::ServiceShutdown));
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc RuntimeSceneService::PublishPendingAggregate */
    Result<void> RuntimeSceneService::PublishPendingAggregate() {
        AggregateTransfer transfer{*this};
        if (aggregateRestore_) {
            if (const auto committed = aggregateRestore_->Commit(transfer); committed.HasError())
                return committed;
        } else {
            transfer.PublishPrepared();
        }
        RetirePublicationReceipt(ScenePublicationStatus::Rejected);
        publicationCheck_.reset();
        initialActivatedChildren_.reset();
        initialStreamedCommands_.reset();
        if (transfer.retiredChildren)
            transfer.retiredChildren->CancelPending();
        transfer.retiredChildren.reset();
        ScenePublicationDetail::ShutdownCandidates(transfer.retired.candidates);
        transfer.retired.restore.reset();
        transfer.retired.scene.reset();
        static_cast<void>(RetirePublishedRestorePreparationInternal());
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::NextRuntimeId */
    SceneRuntimeId RuntimeSceneService::NextRuntimeId() const noexcept {
        HORO_INVARIANT_MSG(std::this_thread::get_id() == restoreRetirementOwner_,
                           "Scene identity cursor belongs to its construction owner lane.");
        return SceneRuntimeId{nextRuntimeId_};
    }

    std::optional<Error> RuntimeSceneService::MakeRestoreRetirementFallback() {
        return MakeError(SceneErrors::InvalidCandidate,
                         "Published Scene preparation retirement remains pending after a cleanup boundary failure.");
    }

    Result<bool> RuntimeSceneService::RetirePublishedRestorePreparationInternal() {
        if (!active_.restore)
            return Result<bool>::Success(true);
        auto *transient = dynamic_cast<ISceneRestoreTransientRetirement *>(active_.restore.get());
        if (!transient)
            return Result<bool>::Success(true);
        try {
            if (!restoreRetirementFallback_)
                restoreRetirementFallback_ = MakeRestoreRetirementFallback();
            auto result = transient->RetirePublishedTransient();
            if (result.HasError())
                operationError_ = result.ErrorValue();
            return result;
        } catch (...) {
            static_assert(std::is_nothrow_swappable_v<std::optional<Error>>);
            if (restoreRetirementFallback_)
                operationError_.swap(restoreRetirementFallback_);
            return Result<bool>::Success(false);
        }
    }

    Result<void> RuntimeSceneService::CommitDeferredChanges(const FrameContext &context) {
        const auto &cancellation = context.cancellation;
        using enum TransitionKind;
        if (structuralCommands_) {
            const auto revision = assetPreparationServices_.registry ? assetPreparationServices_.registry->Snapshot().Revision()
                                                                     : Assets::AssetRegistryRevision{};
            auto admitted = structuralCommands_->ValidateAdmission(active_.scene->View().RuntimeId(), revision);
            auto *trackedResult = structuralReceipt_ ? &structuralReceipt_->result_ : nullptr;
            Result<StructuralCommitResult> committed =
                admitted.HasValue()
                    ? active_.scene->CommitWithRegistry(*structuralCommands_, assetPreparationServices_.registry, structuralParticipants_,
                                                        &operationError_, RuntimeScene::CommitPurpose::Ordinary, trackedResult)
                    : Result<StructuralCommitResult>::Failure(admitted.ErrorValue());
            structuralCommands_.reset();
            if (committed.HasError())
                operationError_ = committed.ErrorValue();
            else if (!structuralReceipt_)
                structuralResult_ = std::move(committed).Value();
            if (structuralReceipt_ && committed.HasError())
                structuralReceipt_->error_ = committed.ErrorValue();
            structuralReceipt_.reset();
        }
        if (transition_ == Activate) {
            const auto prepared = PreparePendingPublication(context);
            if (prepared.HasError()) {
                RejectPendingPublication(prepared.ErrorValue());
                return Result<void>::Success();
            }
            if (!prepared.Value())
                return Result<void>::Success();
            if (const auto valid = ValidateActivatedParentPublication(); valid.HasError()) {
                RejectPendingPublication(valid.ErrorValue());
                return Result<void>::Success();
            }
            if (initialActivatedChildren_ &&
                (initialActivatedChildren_->cancellation.IsCancellationRequested() || context.cancellation.IsCancellationRequested())) {
                RejectPendingPublication(JobCancelled().ErrorValue());
                return Result<void>::Success();
            }
            if (const auto valid = ValidateInitialStreamedAdmission(pending_.scene->View().RuntimeId(), cancellation); valid.HasError()) {
                RejectPendingPublication(valid.ErrorValue());
                return Result<void>::Success();
            }
            if (const auto published = PublishPendingAggregate(); published.HasError()) {
                RejectPendingPublication(published.ErrorValue());
                return Result<void>::Success();
            }
        } else if (transition_ == Unload) {
            if (const auto retired = RetireActiveScene(cancellation); retired.HasError())
                operationError_ = retired.ErrorValue();
        }
        transition_ = None;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::RejectPendingPublication */
    void RuntimeSceneService::RejectPendingPublication(Error error) {
        if (aggregateRestore_)
            aggregateRestore_->Rollback();
        aggregateRestore_.reset();
        operationError_ = std::move(error);
        initialActivatedChildren_.reset();
        ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        initialStreamedCommands_.reset();
        RetirePublicationReceipt(ScenePublicationStatus::Rejected);
        publicationCheck_.reset();
        transition_ = TransitionKind::None;
    }

}  // namespace Horo::Runtime
