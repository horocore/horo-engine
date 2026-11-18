#include "ActivatedSceneInstancesInternal.h"
#include "Horo/Foundation/JobSystem.h"
#include "RuntimeSceneErrors.h"
#include "RuntimeScenePublicationInternal.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Keeps the exact parent epoch and source current through the existing detached Scene preparation. */
        class ActivatedPublicationCheck final : public ScenePublicationCheck {
        public:
            ActivatedPublicationCheck(const RuntimeSceneService &parent, const ActivatedSceneInstanceRequest &request,
                                      std::unique_ptr<ScenePublicationCheck> source) noexcept
                : parent_(parent), request_(request), source_(std::move(source)) {}

            Result<void> ValidatePublication() const override {
                if (request_.parentCancellation.IsCancellationRequested() || request_.cancellation.IsCancellationRequested())
                    return JobCancelled();
                const auto active = parent_.ActiveScene();
                if (!active || active->RuntimeId() != request_.address.parent)
                    return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
                return source_->ValidatePublication();
            }

            Result<void> ValidatePreparedComposition(SceneCanonicalDatasetProjection projection) const override {
                return source_->ValidatePreparedComposition(projection);
            }

        private:
            const RuntimeSceneService &parent_;
            ActivatedSceneInstanceRequest request_;
            std::unique_ptr<ScenePublicationCheck> source_;
        };
    }  // namespace

    namespace Detail {
        bool ActivatedSceneInstances::Matches(const Entry &entry, const ActivatedSceneInstanceOperation &operation) noexcept {
            return entry.address == operation.address_ && entry.identity == operation.child_ &&
                   entry.receipt.state_ == operation.receipt_.state_;
        }

        /** @brief Requires the parent's canonical mutation lane and excludes concurrent parent publication. */
        Result<void> ActivatedSceneInstances::CheckMutation(const RuntimeSceneService &parent, const SceneRuntimeId expected) {
            if (std::this_thread::get_id() != parent.restoreRetirementOwner_ || parent.mutatingLifecycle_ ||
                parent.transition_ != RuntimeSceneService::TransitionKind::None || parent.preparation_)
                return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
            if (parent.shutdown_)
                return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
            const auto active = parent.ActiveScene();
            if (!active || active->RuntimeId() != expected)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            return Result<void>::Success();
        }

        Result<ActivatedSceneInstanceOperation> ActivatedSceneInstances::Queue(
            RuntimeSceneService &parent, const RuntimeSceneDefinition &definition, const ActivatedSceneInstanceRequest &request,
            std::unique_ptr<ScenePublicationCheck> source, std::vector<std::unique_ptr<SceneActivationParticipant>> participants,
            const ActivatedSceneInstanceOperation *replaces) {
            using Outcome = Result<ActivatedSceneInstanceOperation>;
            const auto &address = request.address;
            if (!address.parent.IsValid() || !address.instance.IsValid() || !address.definition.IsValid() ||
                address.definition != definition.Id() || !source || address.mode > SceneInstanceMode::Activated ||
                request.maximumInstances == 0)
                return Outcome::Failure(MakeError(SceneErrors::BaselineInvalid));
            if (address.mode != SceneInstanceMode::Activated)
                return Outcome::Failure(MakeError(SceneErrors::BaselineUnsupported));
            if (request.maximumInstances > MaximumActivatedSceneInstances || participants.size() > MaximumSceneActivationParticipants)
                return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
            if (const auto admitted = CheckMutation(parent, address.parent); admitted.HasError())
                return Outcome::Failure(admitted.ErrorValue());
            if (parent.activationParent_)
                return Outcome::Failure(MakeError(SceneErrors::BaselineUnsupported, "Nested Activated scenes are not admitted"));
            if (parent.ActiveScene()->DefinitionId() == address.definition)
                return Outcome::Failure(MakeError(SceneErrors::BaselineInvalid, "A Scene cannot activate itself"));
            if (request.parentCancellation.IsCancellationRequested() || request.cancellation.IsCancellationRequested())
                return Outcome::Failure(JobCancelled().ErrorValue());
            if (parent.nextRuntimeId_ == 0)
                return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded, "Scene runtime identity space is exhausted"));

            const RuntimeSceneService::LifecycleMutation mutation{parent};
            try {
                auto fresh = parent.activatedInstances_ ? nullptr : std::make_unique<ActivatedSceneInstances>();
                auto &owner = fresh ? *fresh : *parent.activatedInstances_;
                if (owner.pending_ || owner.removal_.IsValid())
                    return Outcome::Failure(MakeError(SceneErrors::OperationInProgress));
                const auto prior = std::ranges::find_if(owner.active_, [&](const Entry &entry) {
                    return entry.address.instance == address.instance;
                });
                if (replaces) {
                    if (prior == owner.active_.end() || prior->address != address || !Matches(*prior, *replaces) ||
                        prior->parentCancellation.IsCancellationRequested() || definition.Revision().value <= prior->revision.value)
                        return Outcome::Failure(MakeError(SceneErrors::BaselineStale));
                } else if (prior != owner.active_.end()) {
                    return Outcome::Failure(MakeError(SceneErrors::BaselineStale));
                }
                const auto replaced = replaces ? prior->identity : SceneRuntimeId{};
                const auto finalSize = owner.active_.size() + (replaces ? 0U : 1U);
                if (finalSize > request.maximumInstances)
                    return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
                owner.active_.reserve(finalSize);

                // This child service accepts exactly one aggregate. Parent and future services cannot reuse its identity.
                const SceneRuntimeId identity{parent.nextRuntimeId_++};
                const auto &assets = parent.assetPreparationServices_;
                auto child = assets.registry && assets.loads
                                 ? std::make_unique<RuntimeSceneService>(*assets.registry, *assets.loads, assets.limits, identity)
                                 : std::make_unique<RuntimeSceneService>(identity);
                child->activationParent_ = &parent;
                for (auto &participant : participants)
                    if (auto added = child->AddActivationParticipant(std::move(participant)); added.HasError())
                        return Outcome::Failure(added.ErrorValue());
                if (auto started = child->Startup(request.parentCancellation); started.HasError())
                    return Outcome::Failure(started.ErrorValue());
                auto check = std::make_unique<ActivatedPublicationCheck>(parent, request, std::move(source));
                auto queued = child->QueuePreparationWithPublicationCheck(definition, std::move(check), request.config);
                if (queued.HasError())
                    return Outcome::Failure(queued.ErrorValue());
                owner.pending_.emplace(
                    Pending{{address, identity, definition.Revision(), request.parentCancellation, std::move(child), queued.Value()},
                            request.cancellation,
                            replaced});
                if (fresh)
                    parent.activatedInstances_ = std::move(fresh);
                return Outcome::Success(ActivatedSceneInstanceOperation{address, identity, std::move(queued).Value()});
            } catch (const std::bad_alloc &) {
                return Outcome::Failure(MakeError(SceneErrors::BaselineCapacityExceeded));
            }
        }

        Result<void> ActivatedSceneInstances::Cancel(RuntimeSceneService &parent, const ActivatedSceneInstanceOperation &operation) {
            if (const auto valid = CheckMutation(parent, operation.address_.parent); valid.HasError())
                return valid;
            const auto &owner = parent.activatedInstances_;
            if (!owner || !owner->pending_ || !Matches(owner->pending_->entry, operation))
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            const RuntimeSceneService::LifecycleMutation mutation{parent};
            owner->CancelPending();
            return Result<void>::Success();
        }

        std::optional<RuntimeSceneView> ActivatedSceneInstances::Find(const RuntimeSceneService &parent,
                                                                      const ActivatedSceneInstanceOperation &operation) noexcept {
            if (std::this_thread::get_id() != parent.restoreRetirementOwner_ || parent.shutdown_ || parent.shutdownDeferred_ ||
                !parent.activatedInstances_)
                return std::nullopt;
            const auto active = parent.ActiveScene();
            if (!active || active->RuntimeId() != operation.address_.parent)
                return std::nullopt;
            const auto &entries = parent.activatedInstances_->active_;
            const auto found = std::ranges::find_if(entries, [&](const Entry &entry) {
                return Matches(entry, operation);
            });
            return found != entries.end() && !found->parentCancellation.IsCancellationRequested() ? found->service->ActiveScene()
                                                                                                  : std::nullopt;
        }

        Result<void> ActivatedSceneInstances::Remove(RuntimeSceneService &parent, const ActivatedSceneInstanceOperation &operation) {
            if (const auto valid = CheckMutation(parent, operation.address_.parent); valid.HasError())
                return valid;
            if (!Find(parent, operation))
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            auto &owner = *parent.activatedInstances_;
            if (owner.pending_ || owner.removal_.IsValid())
                return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
            owner.removal_ = operation.child_;
            return Result<void>::Success();
        }

        void ActivatedSceneInstances::CancelPending() noexcept {
            auto cancelled = std::exchange(pending_, std::nullopt);
            if (cancelled && cancelled->entry.service)
                cancelled->entry.service->CancelPreparation(false);  // Do not wait for AssetLoad jobs at the frame cutoff.
        }

        /** @copydoc ActivatedSceneInstances::ValidateChildPublication */
        Result<void> ActivatedSceneInstances::ValidateChildPublication(const RuntimeSceneService &child) const {
            if (!pending_ || pending_->entry.service.get() != &child)
                return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
            if (pending_->entry.parentCancellation.IsCancellationRequested() || pending_->cancellation.IsCancellationRequested())
                return JobCancelled();
            return Result<void>::Success();
        }

        /** @brief Progresses actual queued child retirement, preserving the resident child after a failed preflight. */
        void ActivatedSceneInstances::AdvanceRemoval(RuntimeSceneService &parent, const FrameContext &context) {
            const auto found = std::ranges::find(active_, std::exchange(removal_, {}), &Entry::identity);
            if (found == active_.end())
                return;
            auto &child = *found->service;
            if (auto queued = child.QueueUnload(); queued.HasError()) {
                parent.operationError_ = std::move(queued).ErrorValue();
                return;
            }
            if (auto progressed = child.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context); progressed.HasError())
                parent.operationError_ = std::move(progressed).ErrorValue();
            if (auto rejected = child.TakeOperationError())
                parent.operationError_ = std::move(rejected);
            if (!child.ActiveScene()) {
                auto retired = std::move(found->service);
                active_.erase(found);
                retired.reset();
            }
        }

        /** @brief Installs only a child whose own existing Scene receipt has actually published. */
        void ActivatedSceneInstances::AdvancePublication(RuntimeSceneService &parent, const FrameContext &context) {
            auto &attempt = *pending_;
            if (attempt.cancellation.IsCancellationRequested() || attempt.entry.parentCancellation.IsCancellationRequested() ||
                context.cancellation.IsCancellationRequested()) {
                CancelPending();
                return;
            }
            const auto prior = std::ranges::find(active_, attempt.replaces, &Entry::identity);
            if (attempt.replaces.IsValid() && prior == active_.end()) {
                CancelPending();
                parent.operationError_ = MakeError(SceneErrors::BaselineStale);
                return;
            }
            auto &child = *attempt.entry.service;
            if (auto advanced = child.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context); advanced.HasError())
                parent.operationError_ = std::move(advanced).ErrorValue();
            if (auto rejected = child.TakeOperationError())
                parent.operationError_ = std::move(rejected);
            const auto receipt = attempt.entry.receipt.Snapshot();
            if (receipt.HasValue() && receipt.Value().status == ScenePublicationStatus::Pending)
                return;
            if (receipt.HasValue() && receipt.Value().status == ScenePublicationStatus::Published && !parent.shutdownDeferred_) {
                if (prior != active_.end())
                    std::swap(*prior, attempt.entry);
                else
                    active_.push_back(std::move(attempt.entry));
            }
            CancelPending();
        }

        void ActivatedSceneInstances::Advance(RuntimeSceneService &parent, const FrameContext &context) {
            // Erase by owner lifetime before progressing an incoming replacement.
            for (std::size_t index = active_.size(); index != 0; --index) {
                auto entry = active_.begin() + static_cast<std::ptrdiff_t>(index - 1);
                if (entry->parentCancellation.IsCancellationRequested()) {
                    auto retired = std::move(entry->service);
                    active_.erase(entry);
                    retired.reset();
                }
            }
            if (parent.shutdownDeferred_)
                return;
            if (removal_.IsValid())
                AdvanceRemoval(parent, context);
            if (pending_ && !parent.shutdownDeferred_)
                AdvancePublication(parent, context);
        }
    }  // namespace Detail

    ActivatedSceneInstanceOperation::ActivatedSceneInstanceOperation(SceneInstanceAddress address, SceneRuntimeId child,
                                                                     ScenePublicationReceipt receipt) noexcept
        : address_(address), child_(child), receipt_(std::move(receipt)) {}

    /** @copydoc ActivatedSceneInstanceOperation::Snapshot */
    Result<ScenePublicationSnapshot> ActivatedSceneInstanceOperation::Snapshot() const {
        return receipt_.Snapshot();
    }

    /** @copydoc ActivatedSceneInstanceOperation::Cancel */
    Result<void> ActivatedSceneInstanceOperation::Cancel(RuntimeSceneService &parent) const {
        return Detail::ActivatedSceneInstances::Cancel(parent, *this);
    }

    /** @copydoc QueueActivatedSceneInstance */
    Result<ActivatedSceneInstanceOperation> QueueActivatedSceneInstance(
        RuntimeSceneService &parent, const RuntimeSceneDefinition &definition, const ActivatedSceneInstanceRequest &request,
        std::unique_ptr<ScenePublicationCheck> source, std::vector<std::unique_ptr<SceneActivationParticipant>> participants,
        const ActivatedSceneInstanceOperation *replaces) {
        return Detail::ActivatedSceneInstances::Queue(parent, definition, request, std::move(source), std::move(participants), replaces);
    }

    /** @copydoc FindActivatedSceneInstance */
    std::optional<RuntimeSceneView> FindActivatedSceneInstance(const RuntimeSceneService &parent,
                                                               const ActivatedSceneInstanceOperation &current) noexcept {
        return Detail::ActivatedSceneInstances::Find(parent, current);
    }

    /** @copydoc QueueActivatedSceneRemoval */
    Result<void> QueueActivatedSceneRemoval(RuntimeSceneService &parent, const ActivatedSceneInstanceOperation &current) {
        return Detail::ActivatedSceneInstances::Remove(parent, current);
    }

    /** @copydoc RuntimeSceneService::ValidateActivatedParentPublication */
    Result<void> RuntimeSceneService::ValidateActivatedParentPublication() const {
        if (!activationParent_)
            return Result<void>::Success();
        if (activationParent_->shutdown_ || activationParent_->shutdownDeferred_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        return activationParent_->activatedInstances_ ? activationParent_->activatedInstances_->ValidateChildPublication(*this)
                                                      : Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
    }

    /** @copydoc RuntimeSceneService::ProgressActivatedInstances */
    void RuntimeSceneService::ProgressActivatedInstances(const FrameContext &context) {
        if (activatedInstances_)
            activatedInstances_->Advance(*this, context);
    }

    /** @copydoc RuntimeSceneService::RetireActivatedInstances */
    void RuntimeSceneService::RetireActivatedInstances() noexcept {
        auto retiring = std::move(activatedInstances_);
        if (retiring)
            retiring->CancelPending();
    }
}  // namespace Horo::Runtime
