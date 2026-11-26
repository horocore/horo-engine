#include "SaveSlotLifecycleInternal.h"
#include "SaveCloudRevisionMetadataInternal.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        using namespace SaveSlotLifecycleDetail;

        /** @brief Admits only the explicitly composed local cloud coordinator. */
        bool CloudAllowed(const SaveSlotLifecyclePolicy &policy) noexcept {
            return (policy.capabilities & (1U << static_cast<unsigned>(SaveSlotLifecycleKind::CloudSync))) != 0;
        }

        /** @brief Recovers the visible pair or derives the baseline for a pre-cloud catalog. */
        Result<SaveCloudRevisionSnapshot> ReadPair(const Catalog &catalog, const SaveCloudMetadataScope &scope) {
            using Response = Result<SaveCloudRevisionSnapshot>;
            auto index = CatalogIndex(catalog);
            if (catalog.cloud) {
                if (catalog.cloud->scope != scope)
                    return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
                return SaveCloudRevisionSnapshot::Create(std::move(index), scope, *catalog.cloud);
            }
            auto metadata = ReconcileSaveCloudRevisionMetadata(index, scope, {}, 1);
            if (metadata.HasError())
                return Response::Failure(metadata.ErrorValue());
            return SaveCloudRevisionSnapshot::Create(std::move(index), scope, std::move(metadata).Value());
        }

        /** @brief Compares the complete catalog consent instead of trusting a revision number alone. */
        bool SameIndex(const SaveSlotIndex &left, const SaveSlotIndex &right) {
            return left.schemaVersion == right.schemaVersion && left.revision == right.revision && left.entries == right.entries;
        }

        /** @brief Fences cloud work to the complete catalog and sidecar selected by its owner. */
        bool SamePair(const SaveCloudRevisionSnapshot &current, const SaveCloudRevisionSnapshot &expected) {
            return SameIndex(current.Index(), expected.Index()) && current.Metadata() == expected.Metadata();
        }

        /** @brief Projects verified archive facts without exposing strings or participant bytes. */
        SaveCloudPublicationHead CloudHead(const ValidatedSaveArchive &archive) noexcept {
            const auto &header = archive.Header();
            return {header.slot, header.slotGeneration, header.parentGeneration, archive.Integrity().archiveContent,
                    archive.Manifest().canonicalState};
        }

        /** @brief Validates the caller's exact prior pair and one in-place cloud revision step. */
        bool CanPublish(const SaveCloudRevisionSnapshot &current, const SaveCloudRevisionSnapshot &expected,
                        const SaveCloudRevisionSnapshot &candidate) {
            if (!SamePair(current, expected) ||
                !SameIndex(current.Index(), candidate.Index()) || candidate.Metadata().scope != current.Metadata().scope)
                return false;
            if (candidate.Metadata() == current.Metadata())
                return true;
            return current.Metadata().revision < std::numeric_limits<std::uint64_t>::max() &&
                   candidate.Metadata().revision == current.Metadata().revision + 1;
        }

        /** @brief Checks that both observations describe one cloud object and a new revision. */
        bool CanReplaceCloudHead(const SaveCloudGenerationRecord &local, const SaveCloudObjectRef &remote) {
            if (local.state == SaveCloudGenerationState::Deleted || local.state == SaveCloudGenerationState::Conflicted)
                return false;
            return !local.object || (local.object->key == remote.key && local.object->revision != remote.revision);
        }

        /** @brief Admits only a verified descendant of the selected publication to the replacement transaction. */
        Result<void> CheckCloudDescendant(const SaveSlotPublicationMetadata &local, const ValidatedSaveArchive &incoming) {
            const auto &header = incoming.Header();
            if (header.slot != local.slot)
                return Result<void>::Failure(WrapError(SaveErrors::CloudArchiveRejected, MakeError(SaveErrors::ArchiveHeaderInvalid)));
            const SaveCloudPublicationHead localHead{local.slot, local.generation, {}, local.archiveContent, local.canonicalState};
            const auto relation = CompareSaveCloudHeads(localHead, CloudHead(incoming));
            if (relation == SaveCloudHeadRelation::IntegrityMismatch)
                return Result<void>::Failure(WrapError(SaveErrors::CloudArchiveRejected, MakeError(SaveErrors::ArchiveContentHashMismatch)));
            if (relation != SaveCloudHeadRelation::RemoteAhead)
                return Result<void>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
            return Result<void>::Success();
        }

        /** @brief Prepares the catalog and its cloud record for the same visibility transaction. */
        Result<void> RecordCloudDescendant(Catalog &catalog, Record &selected, const SaveSlotCatalogEntry &replacement,
                                           const SaveCloudRevisionMetadata &previous, SaveCloudObjectRef object) {
            catalog.retired.emplace_back(selected.entry, false);
            selected.entry = replacement;
            selected.sequence = ++catalog.revision;
            const auto index = CatalogIndex(catalog);
            auto metadata = SaveCloudRevisionMetadataDetail::RebindIndex(index, previous);
            if (metadata.HasError())
                return Result<void>::Failure(metadata.ErrorValue());
            auto next = std::move(metadata).Value();
            ++next.revision;
            const auto record = std::ranges::lower_bound(next.records, replacement.publication.slot, {}, &SaveCloudGenerationRecord::slot);
            if (record == next.records.end() || record->slot != replacement.publication.slot)
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
            record->state = SaveCloudGenerationState::Clean;
            record->object = std::move(object);
            catalog.cloud = std::move(next);
            return Result<void>::Success();
        }
    }

    /** @copydoc SaveSlotLifecycle::ReadCloudRevision */
    Result<SaveCloudRevisionSnapshot> SaveSlotLifecycle::ReadCloudRevision(const SaveNamespaceAccessRequest &access,
                                                                         const SaveCloudMetadataScope &scope) const {
        using Response = Result<SaveCloudRevisionSnapshot>;
        if (!state_ || !CloudAllowed(state_->policy))
            return Response::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        if (scope.localNamespace != access.expected || !scope.IsValid())
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        try {
            Operation operation{*state_};
            if (auto leased = BeginRetention(operation, access); leased.HasError())
                return Response::Failure(leased.ErrorValue());
            return ReadPair(operation.catalog, scope);
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::PublishCloudRevision */
    Result<void> SaveSlotLifecycle::PublishCloudRevision(const SaveNamespaceAccessRequest &access,
                                                        const SaveCloudRevisionSnapshot &expected,
                                                        const SaveCloudRevisionSnapshot &candidate) {
        if (!state_ || !CloudAllowed(state_->policy))
            return Result<void>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto &scope = expected.Metadata().scope;
        if (scope.localNamespace != access.expected)
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        try {
            Operation operation{*state_};
            if (auto leased = BeginRetention(operation, access); leased.HasError())
                return leased;
            auto current = ReadPair(operation.catalog, scope);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!CanPublish(current.Value(), expected, candidate))
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
            if (candidate.Metadata() == current.Value().Metadata()) {
                // A previous replacement may be visible after its directory sync failed.
                return operation.catalog.cloud ? state_->storage.SynchronizeLifecycleCatalog() : Result<void>::Success();
            }
            operation.catalog.cloud = candidate.Metadata();
            return Publish(operation);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::ApplyCloudDescendant */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::ApplyCloudDescendant(
        const SaveSlotLifecycleTarget &target, const SaveCloudRevisionSnapshot &expected, SaveCloudObjectRef object,
        const ImmutableSaveArchive &archive, const CancellationToken &cancellation) {
        using Response = Result<SaveSlotLifecycleResult>;
        if (!state_ || !CloudAllowed(state_->policy))
            return Response::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto &scope = expected.Metadata().scope;
        if (scope.localNamespace != target.address.namespaceAccess.expected || !scope.IsValid() || !target.generation)
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        if (!archive.bytes || archive.bytes->empty() || archive.bytes->size() > state_->policy.archiveLimits.maximumArchiveBytes)
            return Response::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        const SaveCloudRevisionMetadataLimits limits;
        if (object.key.empty() || object.key.size() > limits.maximumObjectKeyBytes || !object.revision ||
            object.revision->empty() || object.revision->size() > limits.maximumRevisionBytes)
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        if (const auto pending = CheckCancellation(cancellation); pending.HasError())
            return Response::Failure(pending.ErrorValue());
        try {
            Operation operation{*state_};
            if (const auto leased = BeginRetention(operation, target.address.namespaceAccess); leased.HasError())
                return Response::Failure(leased.ErrorValue());
            return ApplyCloudDescendantLocked(operation, target, expected, std::move(object), archive, cancellation);
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::InspectCloudDownload */
    Result<SaveCloudConflictSnapshot> SaveSlotLifecycle::InspectCloudDownload(
        const SaveSlotLifecycleTarget &target, const SaveCloudRevisionSnapshot &expected, const ImmutableSaveArchive &archive,
        const CancellationToken &cancellation) const {
        using Response = Result<SaveCloudConflictSnapshot>;
        if (!state_ || !CloudAllowed(state_->policy))
            return Response::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto &scope = expected.Metadata().scope;
        if (scope.localNamespace != target.address.namespaceAccess.expected || !scope.IsValid() || !target.generation)
            return Response::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        const auto limit = state_->policy.archiveLimits.maximumArchiveBytes;
        if (!archive.bytes || archive.bytes->empty() || archive.bytes->size() > limit)
            return Response::Failure(MakeError(SaveErrors::StorageOperationInvalid));
        if (const auto pending = CheckCancellation(cancellation); pending.HasError())
            return Response::Failure(pending.ErrorValue());
        try {
            Operation operation{*state_};
            if (const auto leased = BeginRetention(operation, target.address.namespaceAccess); leased.HasError())
                return Response::Failure(leased.ErrorValue());
            const auto pair = ReadPair(operation.catalog, scope);
            if (pair.HasError())
                return Response::Failure(pair.ErrorValue());
            if (!SamePair(pair.Value(), expected))
                return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
            if (const auto checked = CheckRetentionTarget(operation, target, false); checked.HasError())
                return Response::Failure(checked.ErrorValue());
            return InspectCloudDownloadLocked(target, pair.Value(), archive, cancellation);
        } catch (const std::bad_alloc &) {
            return Response::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveSlotLifecycle::InspectCloudDownloadLocked */
    Result<SaveCloudConflictSnapshot> SaveSlotLifecycle::InspectCloudDownloadLocked(
        const SaveSlotLifecycleTarget &target, const SaveCloudRevisionSnapshot &current, const ImmutableSaveArchive &archive,
        const CancellationToken &cancellation) const {
        using Response = Result<SaveCloudConflictSnapshot>;
        const auto &policy = state_->policy;
        const auto entry = std::ranges::find_if(current.Index().entries, [&target](const SaveSlotCatalogEntry &value) {
            return value.publication.slot == target.address.slot;
        });
        if (entry == current.Index().entries.end())
            return Response::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));
        auto bytes = state_->storage.ReadLifecycleGeneration(*target.generation, policy.archiveLimits.maximumArchiveBytes);
        if (bytes.HasError())
            return Response::Failure(bytes.ErrorValue());
        const auto local = Admit(std::move(bytes).Value(), policy.destination, policy, *state_->host);
        if (local.HasError())
            return Response::Failure(local.ErrorValue());
        if (const auto matched = Matches(*entry, local.Value()); matched.HasError())
            return Response::Failure(matched.ErrorValue());
        const auto remote = Admit(*archive.bytes, policy.destination, policy, *state_->host);
        if (remote.HasError())
            return Response::Failure(WrapError(SaveErrors::CloudArchiveRejected, remote.ErrorValue()));
        const auto localHead = CloudHead(local.Value());
        const auto remoteHead = CloudHead(remote.Value());
        if (localHead.slot != remoteHead.slot)
            return Response::Failure(WrapError(SaveErrors::CloudArchiveRejected, MakeError(SaveErrors::ArchiveHeaderInvalid)));
        if (const auto pending = CheckCancellation(cancellation); pending.HasError())
            return Response::Failure(pending.ErrorValue());
        return Response::Success({.slot = localHead.slot,
                                  .localGeneration = localHead.generation,
                                  .remoteGeneration = remoteHead.generation,
                                  .catalogRevision = current.Index().revision,
                                  .cloudRevision = current.Metadata().revision,
                                  .relation = CompareSaveCloudHeads(localHead, remoteHead),
                                  .equivalentGameplay = localHead.state == remoteHead.state});
    }

    /** @copydoc SaveSlotLifecycle::ApplyCloudDescendantLocked */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::ApplyCloudDescendantLocked(
        Operation &operation, const SaveSlotLifecycleTarget &target, const SaveCloudRevisionSnapshot &expected,
        SaveCloudObjectRef object, const ImmutableSaveArchive &archive, const CancellationToken &cancellation) {
        using Response = Result<SaveSlotLifecycleResult>;
        auto &catalog = operation.catalog;
        const auto current = ReadPair(catalog, expected.Metadata().scope);
        if (current.HasError())
            return Response::Failure(current.ErrorValue());
        const auto &prior = current.Value().Metadata();
        if (!SamePair(current.Value(), expected) || prior.revision == std::numeric_limits<std::uint64_t>::max())
            return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
        const auto local = std::ranges::find(prior.records, target.address.slot, &SaveCloudGenerationRecord::slot);
        if (local == prior.records.end() || !CanReplaceCloudHead(*local, object))
            return Response::Failure(MakeError(SaveErrors::CloudMetadataStale));
        const auto selected = std::ranges::find(catalog.records, target.address.slot, [](const Record &record) {
            return record.entry.publication.slot;
        });
        if (selected == catalog.records.end() || selected->pinned || selected->deleted)
            return Response::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        const auto verified = Admit(*archive.bytes, state_->policy.destination, state_->policy, *state_->host);
        if (verified.HasError())
            return Response::Failure(WrapError(SaveErrors::CloudArchiveRejected, verified.ErrorValue()));
        if (const auto checked = CheckRetentionTarget(operation, target, false); checked.HasError())
            return Response::Failure(checked.ErrorValue());
        if (auto compared = CheckCloudDescendant(selected->entry.publication, verified.Value()); compared.HasError())
            return Response::Failure(compared.ErrorValue());
        if (auto prepared = PrepareRetentionUpdate(operation, target, false); prepared.HasError())
            return Response::Failure(prepared.ErrorValue());
        if (catalog.retired.size() >= state_->policy.maximumSlots)
            return Response::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
        auto replacement = Metadata(verified.Value(), selected->entry.display);
        replacement.publication.kind = selected->entry.publication.kind;
        replacement.publication.checkpoint = selected->entry.publication.checkpoint;
        replacement.publication.cloudState = SaveSlotCloudState::Synchronized;
        if (auto recorded = RecordCloudDescendant(catalog, *selected, replacement, prior, std::move(object)); recorded.HasError())
            return Response::Failure(recorded.ErrorValue());
        const auto encoded = EncodeCatalog(catalog, state_->policy);
        if (encoded.HasError())
            return Response::Failure(encoded.ErrorValue());
        SaveSlotLifecycleResult result{.catalogRevision = catalog.revision, .entry = replacement};
        if (auto published = PublishPrepared(SaveStorageWrite{std::move(replacement), archive}, encoded.Value(), cancellation);
            published.HasError())
            return Response::Failure(published.ErrorValue());
        result.cleanupDeferred = CleanupAfterPublication(operation);
        return Response::Success(std::move(result));
    }
}
