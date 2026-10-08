#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveEventTriggers.h"

#include <algorithm>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc SaveEventTriggers::Resolve */
    Result<SaveTriggerHandoff> SaveEventTriggers::Resolve(const Record &record) const {
        using Return = Result<SaveTriggerHandoff>;
        using enum SaveSlotKind;
        if (const auto context = ValidateContext(record, record.receipt->event); context.HasError())
            return Return::Failure(context.ErrorValue());
        if (const auto access = ValidateSaveNamespaceAccess(record.receipt->access, host_->binding); access.HasError())
            return Return::Failure(access.ErrorValue());
        if (const auto catalog = ValidateSaveSlotIndex(host_->catalog); catalog.HasError())
            return Return::Failure(catalog.ErrorValue());
        const auto kind = record.registration.mode == SavePolicyMode::Auto ? Auto : Checkpoint;
        const auto count = std::ranges::count_if(host_->catalog.entries, [kind](const auto &entry) {
            return entry.publication.kind == kind;
        });
        const auto capacity = policy_.Mode(record.registration.mode)->rotation.maximumRetainedSlots;
        const auto found = std::ranges::find_if(host_->catalog.entries, [&record](const auto &entry) {
            return entry.publication.slot == record.registration.target.slot;
        });
        const bool exists = found != host_->catalog.entries.end();
        if (count > capacity || (!exists && count >= capacity) || (exists && found->publication.kind != kind))
            return Return::Failure(MakeError(SaveErrors::SlotMetadataInvalid));
        return Return::Success({*record.receipt, record.receipt->access, host_->catalog.revision, record.registration.target,
                                exists ? std::optional{found->publication.generation} : std::nullopt});
    }

    /** @copydoc SaveEventTriggers::Revalidate */
    Result<void> SaveEventTriggers::Revalidate(const SaveTriggerHandoff &handoff) const {
        if (const auto owner = ValidateOwner(); owner.HasError())
            return owner;
        const auto found = std::ranges::find_if(records_, [&handoff](const auto &record) {
            return record.receipt && record.receipt->event == handoff.receipt.event &&
                   record.receipt->operation.Id() == handoff.receipt.operation.Id();
        });
        if (found == records_.end() || !handoff.receipt.operation.IsValid())
            return Result<void>::Failure(MakeError(SaveErrors::GenerationStale));
        const auto resolved = Resolve(*found);
        if (resolved.HasError())
            return Result<void>::Failure(resolved.ErrorValue());
        if (const auto &current = resolved.Value();
            current.target != handoff.target || current.access.expected != handoff.access.expected ||
            current.access.expectedRevision != handoff.access.expectedRevision || current.catalogRevision != handoff.catalogRevision ||
            current.expectedGeneration != handoff.expectedGeneration)
            return Result<void>::Failure(MakeError(SaveErrors::GenerationStale));
        return Result<void>::Success();
    }

    /** @copydoc SaveEventTriggers::CommitAtSafePoint */
    Result<std::optional<SaveTriggerHandoff>> SaveEventTriggers::CommitAtSafePoint(const RuntimePhase phase,
                                                                                   SaveOperationDescriptor operation) {
        using Return = Result<std::optional<SaveTriggerHandoff>>;
        if (const auto owner = ValidateOwner(); owner.HasError())
            return Return::Failure(owner.ErrorValue());
        if (phase != RuntimePhase::CommitDeferredLifecycleChanges)
            return Return::Failure(MakeError(SaveErrors::SafePointInvalid));
        if (!pending_.has_value())
            return Return::Success({});
        auto &record = records_[*pending_];
        auto resolved = Resolve(record);
        if (resolved.HasError()) {
            RejectPending(resolved.ErrorValue());
            return Return::Failure(resolved.ErrorValue());
        }
        const auto modeIndex = static_cast<std::size_t>(record.registration.mode);
        if (host_->monotonicMilliseconds < admittedAt_[modeIndex]) {
            const auto error = MakeError(SaveErrors::LifecycleInvalid);
            RejectPending(error);
            return Return::Failure(error);
        }
        if (const auto cooldown = policy_.Mode(record.registration.mode)->cooldown.minimumIntervalMilliseconds;
            (admitted_[modeIndex] && host_->monotonicMilliseconds - admittedAt_[modeIndex] < cooldown) ||
            arbiter_->ActiveOperation().has_value() || arbiter_->QueuedCount() != 0)
            return Return::Success({});
        auto result = Admit(record, std::move(resolved).Value(), std::move(operation));
        if (result.HasError())
            return Return::Failure(result.ErrorValue());
        return Return::Success(std::move(result).Value());
    }

    /** @copydoc SaveEventTriggers::Admit */
    Result<SaveTriggerHandoff> SaveEventTriggers::Admit(Record &record, SaveTriggerHandoff handoff, SaveOperationDescriptor operation) {
        using Return = Result<SaveTriggerHandoff>;
        if (operation.kind != SaveOperationKind::Save)
            return Return::Failure(MakeError(SaveErrors::OperationInvalid));
        auto admitted = arbiter_->Admit({.operation = std::move(operation),
                                         .mode = record.registration.mode,
                                         .address = record.registration.target,
                                         .priority = SaveArbiterPriority::Background,
                                         .conflict = SaveArbiterConflictPolicy::Reject});
        if (admitted.HasError()) {
            RejectPending(admitted.ErrorValue());
            return Return::Failure(admitted.ErrorValue());
        }
        record.receipt->operation = admitted.Value().handle;
        if (const auto fenced =
                safePoints_->Admit({record.receipt->operation.Id(), SaveSafePointAction::Capture, record.receipt->event.generation});
            fenced.HasError()) {
            static_cast<void>(arbiter_->Cancel(record.receipt->operation.Id()));
            RejectPending(fenced.ErrorValue());
            return Return::Failure(fenced.ErrorValue());
        }
        active_ = record.receipt->operation;
        record.receipt->pending = false;
        const auto modeIndex = static_cast<std::size_t>(record.registration.mode);
        admitted_[modeIndex] = true;
        admittedAt_[modeIndex] = host_->monotonicMilliseconds;
        pending_.reset();
        static_cast<void>(arbiter_->StartNext());
        if (const auto advanced = arbiter_->Advance(active_.Id(), SaveArbiterState::WaitingForSafePoint); advanced.HasError())
            return Return::Failure(advanced.ErrorValue());
        handoff.receipt = *record.receipt;
        return Return::Success(std::move(handoff));
    }
}  // namespace Horo::Runtime
