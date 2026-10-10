#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    namespace {
        /** @brief Automatic kinds alone can use retention; all other publications remain explicit CAS commands. */
        [[nodiscard]] bool Automatic(const SaveSlotKind kind) noexcept {
            return kind == SaveSlotKind::Auto || kind == SaveSlotKind::Checkpoint;
        }

        /** @brief Keeps deletion, pinning and category protection independent from rotation ordering. */
        [[nodiscard]] bool Protected(const Record &record, const SaveSlotKind kind) noexcept {
            return record.pinned || record.deleted || record.entry.publication.kind != kind;
        }

        /** @brief Identifies visible peers without interpreting opaque identity bytes as causality. */
        [[nodiscard]] bool Peer(const Record &record, const SaveSlotKind kind) noexcept {
            return !record.deleted && record.entry.publication.kind == kind;
        }

        /** @brief Only known-order unpinned records can be overwritten by automatic rotation. */
        [[nodiscard]] bool Replaceable(const Record &record) noexcept {
            return !record.pinned && record.sequence != 0;
        }

        /** @brief Prevents rotation from overwriting the newest or a pinned/legacy publication. */
        [[nodiscard]] bool CanReplace(const Catalog &catalog, const Record &record, const SaveSlotKind kind) {
            if (Protected(record, kind))
                return false;
            if (!Automatic(kind))
                return true;
            if (record.sequence == 0)
                return false;
            std::uint64_t newest = 0;
            std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
            for (const auto &entry : catalog.records) {
                if (Peer(entry, kind)) {
                    newest = std::max(newest, entry.sequence);
                    if (Replaceable(entry))
                        oldest = std::min(oldest, entry.sequence);
                }
            }
            return record.sequence == oldest && record.sequence != newest;
        }

        /** @brief Admits finalized metadata independently from advisory display and archive-selected identity. */
        [[nodiscard]] Result<void> ValidateCandidate(const SaveSlotLifecycleTarget &target, const SaveStorageWrite &candidate) {
            if (!candidate.archive.bytes || candidate.archive.bytes->empty() ||
                candidate.metadata.publication.slot != target.address.slot ||
                candidate.metadata.publication.generation == target.generation)
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
            if (auto valid = ValidateSaveSlotPublicationMetadata(candidate.metadata.publication); valid.HasError())
                return valid;
            return ValidateSaveSlotDisplayMetadata(candidate.metadata.display);
        }

        /** @brief Requires a validated sidecar for the exact pre-mutation catalog, not a stale coordinator projection. */
        [[nodiscard]] Result<void> CheckCloud(const Catalog &catalog, const SaveNamespaceId &name, const SaveCloudRevisionSnapshot *cloud) {
            if (!cloud)
                return Result<void>::Success();
            SaveSlotIndex index{.revision = catalog.revision};
            for (const auto &record : catalog.records) {
                if (!record.deleted)
                    index.entries.push_back(record.entry);
            }
            if (cloud->Metadata().scope.localNamespace != name || cloud->Index().revision != index.revision ||
                cloud->Index().entries != index.entries)
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
            return Result<void>::Success();
        }

        /** @brief Binds every new durable tombstone to exact provider object evidence before any storage writes. */
        [[nodiscard]] Result<void> BindCloud(Catalog &catalog, const SaveCloudRevisionSnapshot *cloud) {
            for (auto &record : catalog.retired) {
                if (!record.tombstone || record.cloud)
                    continue;
                if (!cloud)
                    return Result<void>::Failure(MakeError(SaveErrors::StorageResultInvalid));
                const auto generation = std::ranges::find(cloud->Metadata().records, record.entry.publication.generation,
                                                          &SaveCloudGenerationRecord::generation);
                if (generation == cloud->Metadata().records.end())
                    return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
                record.cloud = SaveSlotRetentionCloudDeletion{cloud->Metadata().scope, *generation};
            }
            return Result<void>::Success();
        }

        /** @brief Separates immutable host authority and input bounds from worker publication ownership. */
        [[nodiscard]] Result<void> AuthorizePublication(const SaveSlotLifecyclePolicy &policy, const SaveSlotLifecycleTarget &target,
                                                        const SaveStorageWrite &candidate) {
            if ((policy.capabilities & (1U << static_cast<unsigned>(SaveSlotLifecycleKind::PublishSave))) == 0)
                return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            if (Automatic(candidate.metadata.publication.kind)) {
                if (!policy.retention.enabled ||
                    (policy.capabilities & (1U << static_cast<unsigned>(SaveSlotLifecycleKind::Retention))) == 0)
                    return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            }
            if (auto valid = ValidateCandidate(target, candidate); valid.HasError())
                return valid;
            if (candidate.archive.bytes->size() > policy.archiveLimits.maximumArchiveBytes)
                return Result<void>::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SaveSlotLifecycle::CommitSave */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::CommitSave(const SaveSlotLifecycleTarget &target, SaveStorageWrite candidate,
                                                                  const std::uint64_t committedAtMilliseconds, const bool lowSpace,
                                                                  const CancellationToken &cancellation,
                                                                  const SaveCloudRevisionSnapshot *cloud) {
        if (!state_)
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        try {
            if (auto authorized = AuthorizePublication(state_->policy, target, candidate); authorized.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(authorized.ErrorValue());
            Operation operation{*state_};
            auto lease = BeginRetention(operation, target.address.namespaceAccess);
            if (lease.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(lease.ErrorValue());
            if (auto checked = CheckRetentionTarget(operation, target, false); checked.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(checked.ErrorValue());
            if (auto cleaned = Cleanup(operation); cleaned.HasError() || cleaned.Value())
                return Result<SaveSlotLifecycleResult>::Failure(cleaned.HasError() ? cleaned.ErrorValue()
                                                                                   : MakeError(SaveErrors::OperationInProgress));
            return CommitSaveLocked(operation, target, std::move(candidate), committedAtMilliseconds, lowSpace, cancellation, cloud);
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::PrepareRetentionPublication */
    Result<void> SaveSlotLifecycle::PrepareRetentionPublication(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                                const SaveStorageWrite &candidate, const std::uint64_t clock,
                                                                const bool lowSpace, SaveSlotLifecycleResult &result) {
        auto &catalog = operation.catalog;
        const auto kind = candidate.metadata.publication.kind;
        auto selected = std::ranges::find(catalog.records, target.address.slot, [](const Record &record) {
            return record.entry.publication.slot;
        });
        if (selected != catalog.records.end()) {
            if (!CanReplace(catalog, *selected, kind))
                return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            if (Automatic(kind)) {
                result.retention.push_back({selected->entry, SaveSlotRetentionReason::Replacement, selected->sequence});
                RetireForRetention(catalog, *selected, state_->policy.retention);
            } else {
                catalog.retired.push_back({selected->entry, false});
            }
            catalog.records.erase(selected);
        }
        catalog.records.push_back({candidate.metadata, false, result.catalogRevision, clock, false});
        catalog.revision = result.catalogRevision;
        catalog.clock = clock;
        if (Automatic(kind)) {
            auto decisions = ApplyRetention(catalog, kind, state_->policy.retention, lowSpace);
            if (decisions.HasError())
                return Result<void>::Failure(decisions.ErrorValue());
            auto &values = decisions.Value();
            result.retention.insert(result.retention.end(), std::make_move_iterator(values.begin()), std::make_move_iterator(values.end()));
        }
        if (catalog.records.size() > state_->policy.maximumSlots || catalog.retired.size() > state_->policy.maximumSlots)
            return Result<void>::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
        return Result<void>::Success();
    }

    /** @copydoc SaveSlotLifecycle::CommitSaveLocked */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::CommitSaveLocked(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                                        SaveStorageWrite candidate, const std::uint64_t clock,
                                                                        const bool lowSpace, const CancellationToken &cancellation,
                                                                        const SaveCloudRevisionSnapshot *cloud) {
        auto &catalog = operation.catalog;
        if (clock < catalog.clock || catalog.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        auto admitted = Admit(*candidate.archive.bytes, state_->policy.destination, state_->policy, *state_->host);
        if (admitted.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(admitted.ErrorValue());
        if (admitted.Value().Header().parentGeneration != target.generation)
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
        if (auto matched = Matches(candidate.metadata, admitted.Value()); matched.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(matched.ErrorValue());
        if (auto checked = CheckCloud(catalog, state_->policy.destination.name, cloud); checked.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(checked.ErrorValue());
        SaveSlotLifecycleResult result{.catalogRevision = catalog.revision + 1, .entry = candidate.metadata};
        if (auto prepared = PrepareRetentionPublication(operation, target, candidate, clock, lowSpace, result); prepared.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(prepared.ErrorValue());
        if (auto bound = BindCloud(catalog, cloud); bound.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(bound.ErrorValue());
        std::ranges::sort(catalog.records, {}, [](const Record &record) {
            return record.entry.publication.slot;
        });
        auto encoded = EncodeCatalog(catalog, state_->policy);
        if (encoded.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(encoded.ErrorValue());
        if (auto published = PublishPrepared(candidate, encoded.Value(), cancellation); published.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(std::move(published).ErrorValue());
        result.cleanupDeferred = CleanupAfterPublication(operation);
        return Result<SaveSlotLifecycleResult>::Success(std::move(result));
    }
}  // namespace Horo::Runtime
