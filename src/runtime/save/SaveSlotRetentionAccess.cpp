#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    namespace {
        /** @brief Preserves independent host capability authorization for retention commands. */
        [[nodiscard]] bool Allowed(const SaveSlotLifecyclePolicy &policy, const SaveSlotLifecycleKind kind) noexcept {
            return (policy.capabilities & (1U << static_cast<unsigned>(kind))) != 0;
        }
    }  // namespace

    /** @copydoc SaveSlotLifecycle::BeginRetention */
    Result<void> SaveSlotLifecycle::BeginRetention(Operation &operation, const SaveNamespaceAccessRequest &access) const {
        if (access.expected != state_->policy.destination.name)
            return Result<void>::Failure(MakeError(SaveErrors::NamespaceStale));
        auto lease = state_->host->AcquireBinding(access);
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        if (!lease.Value())
            return Result<void>::Failure(MakeError(SaveErrors::StorageResultInvalid));
        operation.retentionLease_ = std::move(lease).Value();
        return Load(operation);
    }

    /** @copydoc SaveSlotLifecycle::CheckRetentionTarget */
    Result<void> SaveSlotLifecycle::CheckRetentionTarget(const Operation &operation, const SaveSlotLifecycleTarget &target,
                                                         const bool retained) const {
        if (!target.address.slot.IsValid() || target.catalogRevision != operation.catalog.revision ||
            (target.generation && !target.generation->IsValid()))
            return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
        if (retained) {
            const bool found = std::ranges::any_of(operation.catalog.retired, [&target](const Retired &record) {
                return record.entry.publication.slot == target.address.slot && record.entry.publication.generation == target.generation;
            });
            return found ? Result<void>::Success() : Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
        }
        const auto record = std::ranges::find(operation.catalog.records, target.address.slot, [](const Record &value) {
            return value.entry.publication.slot;
        });
        const auto generation =
            record == operation.catalog.records.end() ? std::optional<SlotGenerationId>{} : record->entry.publication.generation;
        return generation == target.generation ? Result<void>::Success()
                                               : Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
    }

    /** @copydoc SaveSlotLifecycle::PrepareRetentionUpdate */
    Result<void> SaveSlotLifecycle::PrepareRetentionUpdate(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                           const bool retained) const {
        if (!target.generation || operation.catalog.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
        if (auto cleaned = Cleanup(operation); cleaned.HasError() || cleaned.Value())
            return Result<void>::Failure(cleaned.HasError() ? cleaned.ErrorValue() : MakeError(SaveErrors::OperationInProgress));
        return CheckRetentionTarget(operation, target, retained);
    }

    /** @copydoc SaveSlotLifecycle::RetentionSnapshot */
    Result<SaveSlotRetentionSnapshot> SaveSlotLifecycle::RetentionSnapshot(const SaveNamespaceAccessRequest &access) const {
        if (!state_)
            return Result<SaveSlotRetentionSnapshot>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        try {
            Operation operation{*state_};
            if (auto lease = BeginRetention(operation, access); lease.HasError())
                return Result<SaveSlotRetentionSnapshot>::Failure(lease.ErrorValue());
            SaveSlotRetentionSnapshot snapshot{operation.catalog.revision, operation.catalog.clock, {}, {}};
            for (const auto &record : operation.catalog.records)
                snapshot.selected.emplace_back(record.entry, record.sequence, record.committedAt, record.pinned, record.deleted, false,
                                               false);
            for (const auto &record : operation.catalog.retired)
                snapshot.retained.emplace_back(record.entry, record.sequence, record.committedAt, false, false, record.backup,
                                               record.tombstone, record.cloud);
            return Result<SaveSlotRetentionSnapshot>::Success(std::move(snapshot));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotRetentionSnapshot>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::SetPinned */
    Result<std::uint64_t> SaveSlotLifecycle::SetPinned(const SaveSlotLifecycleTarget &target, const bool pinned) {
        if (!state_ || !Allowed(state_->policy, SaveSlotLifecycleKind::Retention))
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        try {
            Operation operation{*state_};
            if (auto lease = BeginRetention(operation, target.address.namespaceAccess); lease.HasError())
                return Result<std::uint64_t>::Failure(lease.ErrorValue());
            if (auto checked = PrepareRetentionUpdate(operation, target, false); checked.HasError())
                return Result<std::uint64_t>::Failure(checked.ErrorValue());
            const auto record = std::ranges::find(operation.catalog.records, target.address.slot, [](const Record &value) {
                return value.entry.publication.slot;
            });
            if (record->deleted)
                return Result<std::uint64_t>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
            record->pinned = pinned;
            ++operation.catalog.revision;
            if (auto published = Publish(operation); published.HasError())
                return Result<std::uint64_t>::Failure(published.ErrorValue());
            return Result<std::uint64_t>::Success(operation.catalog.revision);
        } catch (const std::bad_alloc &) {
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::ReadBackup */
    Result<ImmutableSaveArchive> SaveSlotLifecycle::ReadBackup(const SaveSlotLifecycleTarget &target) const {
        if (!state_ || !Allowed(state_->policy, SaveSlotLifecycleKind::Export))
            return Result<ImmutableSaveArchive>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        try {
            Operation operation{*state_};
            if (auto lease = BeginRetention(operation, target.address.namespaceAccess); lease.HasError())
                return Result<ImmutableSaveArchive>::Failure(lease.ErrorValue());
            if (auto checked = CheckRetentionTarget(operation, target, true); checked.HasError())
                return Result<ImmutableSaveArchive>::Failure(checked.ErrorValue());
            const auto record = std::ranges::find(operation.catalog.retired, *target.generation, [](const Retired &value) {
                return value.entry.publication.generation;
            });
            if (!record->backup)
                return Result<ImmutableSaveArchive>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            auto bytes = state_->storage.ReadLifecycleGeneration(*target.generation, state_->policy.archiveLimits.maximumArchiveBytes);
            if (bytes.HasError())
                return Result<ImmutableSaveArchive>::Failure(bytes.ErrorValue());
            auto archive = Admit(bytes.Value(), state_->policy.destination, state_->policy, *state_->host);
            if (archive.HasError())
                return Result<ImmutableSaveArchive>::Failure(archive.ErrorValue());
            if (auto matched = Matches(record->entry, archive.Value()); matched.HasError())
                return Result<ImmutableSaveArchive>::Failure(matched.ErrorValue());
            return Result<ImmutableSaveArchive>::Success({std::make_shared<const std::vector<std::byte>>(std::move(bytes).Value())});
        } catch (const std::bad_alloc &) {
            return Result<ImmutableSaveArchive>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::AcknowledgeCloudDelete */
    Result<std::uint64_t> SaveSlotLifecycle::AcknowledgeCloudDelete(const SaveSlotLifecycleTarget &target,
                                                                    const SaveCloudMetadataScope &scope,
                                                                    const SaveCloudMutationId confirmed) {
        if (!state_ || !Allowed(state_->policy, SaveSlotLifecycleKind::Retention) || !confirmed.IsValid())
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        try {
            Operation operation{*state_};
            if (auto lease = BeginRetention(operation, target.address.namespaceAccess); lease.HasError())
                return Result<std::uint64_t>::Failure(lease.ErrorValue());
            if (auto checked = PrepareRetentionUpdate(operation, target, true); checked.HasError())
                return Result<std::uint64_t>::Failure(checked.ErrorValue());
            const auto record = std::ranges::find(operation.catalog.retired, *target.generation, [](const Retired &value) {
                return value.entry.publication.generation;
            });
            if (!record->tombstone || !record->cloud || record->cloud->scope != scope)
                return Result<std::uint64_t>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
            record->tombstone = false;
            record->cloud->generation.state = SaveCloudGenerationState::Deleted;
            record->cloud->generation.lastConfirmed = confirmed;
            ++operation.catalog.revision;
            if (auto published = Publish(operation); published.HasError())
                return Result<std::uint64_t>::Failure(published.ErrorValue());
            // Acknowledgement only releases the cloud hold. Worker reconciliation performs cleanup separately.
            return Result<std::uint64_t>::Success(operation.catalog.revision);
        } catch (const std::bad_alloc &) {
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
