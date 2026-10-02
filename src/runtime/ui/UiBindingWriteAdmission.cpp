#include "UiBindingStoreInternal.h"

#include <limits>

namespace Horo::Runtime::Ui {
    using BindingStoreInternal::Failure;

    namespace {
        /** @brief Matches exact document/tree ownership while presentation validity remains the caller's explicit prerequisite. */
        [[nodiscard]] bool SameTreeOwner(const UiElementTree &tree, const UiActionOwnerContext &owner) noexcept {
            return owner.instance == tree.Instance() && owner.canvas == tree.Canvas() && owner.document == tree.SourceDocument() &&
                   owner.documentRevision == tree.SourceDocumentRevision() && owner.treeRevision == tree.Revision();
        }
    }  // namespace

    /** @copydoc UiBindingStore::Storage::FindTarget */
    UiBindingStore::Storage::Target *UiBindingStore::Storage::FindTarget(const UiBindingId binding) noexcept {
        const auto found = std::ranges::find(targets, binding, [](const Target &target) {
            return target.bound.binding;
        });
        return found == targets.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc UiBindingStore::Storage::ValidateWriteSource */
    Result<void> UiBindingStore::Storage::ValidateWriteSource(const UiElementTree &tree, const Target &target,
                                                              const UiActionSource &source) const {
        if (const auto valid = ValidateTree(tree); valid.HasError())
            return valid;
        if (!target.admission || !source.IsValid() || source.owner != target.admission->owner || source.element != target.bound.element)
            return Failure(UiErrors::RevisionStale);
        if (!SameTreeOwner(tree, source.owner))
            return Failure(UiErrors::RevisionStale);
        if (!tree.Get(source.element).HasValue())
            return Failure(UiErrors::HandleStale);
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::AdmitWrites */
    Result<void> UiBindingStore::AdmitWrites(const UiElementTree &tree, const std::span<const UiBindingWriteAdmission> admissions) {
        if (!storage_)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return valid;
        if (storage_->writesAdmitted || admissions.size() > storage_->targets.size())
            return Failure(UiErrors::BindingDescriptorConflict);
        for (std::size_t index = 0; index < admissions.size(); ++index) {
            const auto &admission = admissions[index];
            const auto *target = storage_->FindTarget(admission.binding);
            if (!target || !admission.authority || !admission.action.IsValid() || !admission.owner.IsValid() ||
                admission.trigger >= UiBindingCommitTrigger::Count || admission.conflict != UiBindingConflictPolicy::RejectStale)
                return Failure(UiErrors::BindingAccessInvalid);
            const auto &provider = storage_->providers[target->provider];
            const auto &property = provider.schema.Properties()[target->property];
            if (property.type != UiBindingValueType::Boolean && property.type != UiBindingValueType::FixedScalar &&
                property.type != UiBindingValueType::BoundedText)
                return Failure(UiErrors::BindingTypeMismatch);
            if (const auto &fence = admission.authority->Fence();
                target->direction == UiBindingDirection::SourceToTarget || property.access == UiBindingAccess::Read ||
                HasFlag(provider.schema.Flags(), UiBindingProviderFlags::Immutable) || fence.capability == 0 ||
                fence.provider != provider.instance || fence.scope != provider.scope || fence.schema != provider.schema.Version() ||
                fence.property != target->property || fence.signature != property.signatureFingerprint)
                return Failure(UiErrors::BindingAccessInvalid);
            if (!SameTreeOwner(tree, admission.owner))
                return Failure(UiErrors::RevisionStale);
            for (std::size_t previous = 0; previous < index; ++previous)
                if (admissions[previous].binding == admission.binding)
                    return Failure(UiErrors::BindingDescriptorConflict);
        }
        for (const auto &admission : admissions) {
            auto *target = storage_->FindTarget(admission.binding);
            target->admission = admission;
            target->fence = admission.authority->Fence();
        }
        storage_->writesAdmitted = true;
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::UpdateWritePresentation */
    Result<void> UiBindingStore::UpdateWritePresentation(const UiElementTree &tree, const UiActionOwnerContext &owner) {
        if (!storage_ || storage_->processingWrite)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        if (const auto valid = storage_->ValidateTree(tree); valid.HasError())
            return valid;
        if (!owner.IsValid() || !SameTreeOwner(tree, owner))
            return Failure(UiErrors::RevisionStale);
        for (const auto &target : storage_->targets)
            if (target.admission && owner.interaction <= target.admission->owner.interaction)
                return Failure(UiErrors::RevisionStale);
        for (auto &target : storage_->targets)
            if (target.admission) {
                storage_->CancelWrite(target, UiBindingWriteCancellationReason::PresentationChanged);
                target.admission->owner = owner;
            }
        return Result<void>::Success();
    }

    /** @copydoc UiBindingStore::BeginEdit */
    Result<UiBindingEditId> UiBindingStore::BeginEdit(const UiElementTree &tree, const UiBindingId binding, const UiActionSource &source) {
        if (!storage_ || !storage_->active)
            return Failure<UiBindingEditId>(UiErrors::BindingLifecycleUnavailable);
        auto *target = storage_->FindTarget(binding);
        if (!target || !target->admission)
            return Failure<UiBindingEditId>(UiErrors::BindingAccessInvalid);
        if (const auto valid = storage_->ValidateWriteSource(tree, *target, source); valid.HasError())
            return Result<UiBindingEditId>::Failure(valid.ErrorValue());
        if (!storage_->providers[target->provider].active || storage_->providers[target->provider].writesRevoked ||
            !target->admission->authority->Active())
            return Failure<UiBindingEditId>(UiErrors::BindingLifecycleUnavailable);
        if (target->command || target->edit.sequence != 0 || storage_->processingWrite)
            return Failure<UiBindingEditId>(UiErrors::BindingDescriptorConflict);
        if (storage_->editSequence == std::numeric_limits<std::uint64_t>::max())
            return Failure<UiBindingEditId>(UiErrors::RevisionInvalid);
        target->edit = {tree.Instance().ownership, ++storage_->editSequence, target->bound.element};
        target->source = source;
        target->expected = storage_->providers[target->provider].revision;
        return Result<UiBindingEditId>::Success(target->edit);
    }

    /** @copydoc UiBindingStore::CancelEdit */
    Result<void> UiBindingStore::CancelEdit(const UiBindingEditId &edit) {
        if (!storage_ || storage_->processingWrite)
            return Failure(UiErrors::BindingLifecycleUnavailable);
        const auto found = std::ranges::find(storage_->targets, edit, &Storage::Target::edit);
        if (edit.sequence == 0 || found == storage_->targets.end())
            return Failure(UiErrors::RevisionStale);
        found->edit = {};
        return Result<void>::Success();
    }

}  // namespace Horo::Runtime::Ui
