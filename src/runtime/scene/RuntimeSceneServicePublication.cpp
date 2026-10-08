#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeScenePublicationInternal.h"

#include <utility>

namespace Horo::Runtime {
    /** @brief Private no-fail transfer retaining the retired Scene through save operation completion. */
    struct RuntimeSceneService::AggregateTransfer final : IStagedRestoreAggregatePublication {
        explicit AggregateTransfer(RuntimeSceneService &service) noexcept : service(service) {}

        void PublishPrepared() noexcept override {
            for (const auto &candidate : service.pending_.candidates)
                candidate->Publish();
            service.pending_.restore = std::move(service.aggregateRestore_);
            retired = std::move(service.active_);
            service.active_ = std::move(service.pending_);
            if (service.publicationReceipt_) {
                const auto view = service.active_.scene->View();
                service.publicationReceipt_->snapshot = {ScenePublicationStatus::Published, view.RuntimeId(), view.StructuralRevision(),
                                                         service.active_.datasets};
            }
        }

        RuntimeSceneService &service;
        SceneAggregate retired;
    };

    /** @copydoc RuntimeSceneService::PreparePendingPublication */
    Result<bool> RuntimeSceneService::PreparePendingPublication() {
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
        ShutdownCandidates(transfer.retired.candidates);
        transfer.retired.restore.reset();
        transfer.retired.scene.reset();
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::CommitDeferredChanges() {
        using enum TransitionKind;
        if (structuralCommands_) {
            const auto revision = assetRegistry_ ? assetRegistry_->Snapshot().Revision() : Assets::AssetRegistryRevision{};
            auto admitted = structuralCommands_->ValidateAdmission(active_.scene->View().RuntimeId(), revision);
            Result<StructuralCommitResult> committed =
                admitted.HasValue()
                    ? active_.scene->CommitWithRegistry(*structuralCommands_, assetRegistry_, structuralParticipants_, &operationError_)
                    : Result<StructuralCommitResult>::Failure(admitted.ErrorValue());
            structuralCommands_.reset();
            if (committed.HasError())
                operationError_ = committed.ErrorValue();
            else if (!structuralReceipt_)
                structuralResult_ = std::move(committed).Value();
            if (structuralReceipt_) {
                if (committed.HasError())
                    structuralReceipt_->error_ = committed.ErrorValue();
                else
                    structuralReceipt_->result_ = std::move(committed).Value();
                structuralReceipt_.reset();
            }
        }
        if (transition_ == Activate) {
            const auto prepared = PreparePendingPublication();
            if (prepared.HasError()) {
                RejectPendingPublication(prepared.ErrorValue());
                return Result<void>::Success();
            }
            if (!prepared.Value())
                return Result<void>::Success();
            if (const auto published = PublishPendingAggregate(); published.HasError()) {
                RejectPendingPublication(published.ErrorValue());
                return Result<void>::Success();
            }
        } else if (transition_ == Unload) {
            SceneAggregate retired = std::move(active_);
            ShutdownCandidates(retired.candidates);
            retired.restore.reset();
            retired.scene.reset();
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
        ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        RetirePublicationReceipt(ScenePublicationStatus::Rejected);
        publicationCheck_.reset();
        transition_ = TransitionKind::None;
    }

}  // namespace Horo::Runtime
