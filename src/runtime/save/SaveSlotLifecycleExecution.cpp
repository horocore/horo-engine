#include "SaveSlotLifecycleInternal.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    /** @copydoc CheckCancellation */
    Result<void> CheckCancellation(const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(SaveErrors::OperationCancelled));
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail

namespace Horo::Runtime {
    using namespace SaveSlotLifecycleDetail;

    /** @copydoc SaveSlotLifecycle::ExecuteLocked */
    Result<SaveSlotLifecycleResult> SaveSlotLifecycle::ExecuteLocked(Operation &operation, const SaveSlotLifecycleRequest &request,
                                                                     const CancellationToken &cancellation) {
        auto &catalog = operation.catalog;
        std::vector<std::byte> bytes;
        auto admitted = AcquireArchive(operation, request, bytes);
        if (admitted.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(admitted.ErrorValue());
        if (request.kind == SaveSlotLifecycleKind::Export) {
            if (auto cancelled = CheckCancellation(cancellation); cancelled.HasError())
                return Result<SaveSlotLifecycleResult>::Failure(cancelled.ErrorValue());
            const auto source = std::ranges::find(catalog.records, request.source.address.slot, [](const Record &record) {
                return record.entry.publication.slot;
            });
            return Result<SaveSlotLifecycleResult>::Success(
                {.catalogRevision = catalog.revision,
                 .entry = source->entry,
                 .exported = {std::make_shared<const std::vector<std::byte>>(std::move(bytes))}});
        }
        if (catalog.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<SaveSlotLifecycleResult>::Failure(MakeError(SaveErrors::SlotCommitInvalid));
        SaveSlotLifecycleResult result{.catalogRevision = catalog.revision + 1};
        auto prepared = PrepareMutation(operation, request, admitted.Value(), result);
        if (prepared.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(prepared.ErrorValue());
        catalog.revision = result.catalogRevision;
        std::ranges::sort(catalog.records, {}, [](const Record &record) {
            return record.entry.publication.slot;
        });
        // All result allocation/encoding finishes before candidate staging and the selection gate.
        auto encoded = EncodeCatalog(catalog, state_->policy);
        if (encoded.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(encoded.ErrorValue());
        if (auto cancelled = CheckCancellation(cancellation); cancelled.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(cancelled.ErrorValue());
        if (auto published = PublishPrepared(prepared.Value(), encoded.Value(), cancellation); published.HasError())
            return Result<SaveSlotLifecycleResult>::Failure(std::move(published).ErrorValue());
        // Publication is acknowledged; fallible cleanup cannot relabel it as unchanged-state failure.
        try {
            auto cleaned = Cleanup(operation);
            result.cleanupDeferred = cleaned.HasError() || cleaned.Value();
        } catch (const std::bad_alloc &) {
            result.cleanupDeferred = true;
        } catch (...) {
            result.cleanupDeferred = true;
        }
        return Result<SaveSlotLifecycleResult>::Success(std::move(result));
    }

    /** @copydoc SaveSlotLifecycle::AcquireArchive */
    Result<ValidatedSaveArchive> SaveSlotLifecycle::AcquireArchive(const Operation &operation, const SaveSlotLifecycleRequest &request,
                                                                   std::vector<std::byte> &bytes) const {
        auto &catalog = operation.catalog;
        const auto source = std::ranges::find(catalog.records, request.source.address.slot, [](const Record &record) {
            return record.entry.publication.slot;
        });
        const bool importing = request.kind == SaveSlotLifecycleKind::Import;
        const bool restore = request.kind == SaveSlotLifecycleKind::RestoreDeleted;
        if (const bool purging = request.kind == SaveSlotLifecycleKind::Delete && request.deleteMode != SaveSlotDeleteMode::Soft;
            !importing && (source == catalog.records.end() || (restore && !source->deleted) || (!restore && !purging && source->deleted)))
            return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::SlotCommitGenerationStale));

        const auto &scope = importing ? state_->policy.importSources[*request.importSource] : state_->policy.destination;
        if (importing) {
            if (scope.name != state_->policy.destination.name && source != catalog.records.end())
                return Result<ValidatedSaveArchive>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
            bytes = *request.imported.bytes;
        } else {
            auto read = state_->storage.ReadLifecycleGeneration(source->entry.publication.generation,
                                                                state_->policy.archiveLimits.maximumArchiveBytes);
            if (read.HasError())
                return Result<ValidatedSaveArchive>::Failure(read.ErrorValue());
            bytes = std::move(read).Value();
        }
        auto admitted = Admit(bytes, scope, state_->policy, *state_->host);
        if (admitted.HasError())
            return Result<ValidatedSaveArchive>::Failure(admitted.ErrorValue());
        if (!importing) {
            if (auto matched = Matches(source->entry, admitted.Value()); matched.HasError())
                return Result<ValidatedSaveArchive>::Failure(matched.ErrorValue());
        }
        return admitted;
    }

    /** @copydoc SaveSlotLifecycle::PrepareMutation */
    Result<std::optional<SaveStorageWrite>> SaveSlotLifecycle::PrepareMutation(Operation &operation,
                                                                               const SaveSlotLifecycleRequest &request,
                                                                               const ValidatedSaveArchive &archive,
                                                                               SaveSlotLifecycleResult &result) {
        auto &catalog = operation.catalog;
        const auto source = std::ranges::find(catalog.records, request.source.address.slot, [](const Record &record) {
            return record.entry.publication.slot;
        });
        const bool importing = request.kind == SaveSlotLifecycleKind::Import;
        std::optional<SaveStorageWrite> prepared;
        if (importing || request.kind == SaveSlotLifecycleKind::Copy) {
            const auto &target = importing ? request.source : *request.destination;
            auto write = PrepareCopy(operation, target, archive, request.display);
            if (write.HasError())
                return Result<std::optional<SaveStorageWrite>>::Failure(write.ErrorValue());
            prepared = std::move(write).Value();
            result.entry = prepared->metadata;
        } else if (request.kind == SaveSlotLifecycleKind::Rename) {
            source->entry.display = request.display;
            result.entry = source->entry;
        } else if (request.kind == SaveSlotLifecycleKind::RestoreDeleted) {
            source->deleted = false;
            result.entry = source->entry;
        } else if (request.deleteMode == SaveSlotDeleteMode::Soft) {
            source->deleted = true;
            result.entry = source->entry;
        } else {
            catalog.retired.emplace_back(source->entry, request.deleteMode == SaveSlotDeleteMode::PlatformRecycle);
            catalog.records.erase(source);
        }
        return Result<std::optional<SaveStorageWrite>>::Success(std::move(prepared));
    }

    /** @copydoc SaveSlotLifecycle::PrepareCopy */
    Result<SaveStorageWrite> SaveSlotLifecycle::PrepareCopy(Operation &operation, const SaveSlotLifecycleTarget &target,
                                                            const ValidatedSaveArchive &archive, const SaveSlotDisplayMetadata &display) {
        auto &catalog = operation.catalog;
        const auto destination = std::ranges::find(catalog.records, target.address.slot, [](const Record &record) {
            return record.entry.publication.slot;
        });
        if (destination != catalog.records.end() && destination->deleted)
            return Result<SaveStorageWrite>::Failure(MakeError(SaveErrors::StoragePermissionDenied));
        if (destination == catalog.records.end() && catalog.records.size() >= state_->policy.maximumSlots)
            return Result<SaveStorageWrite>::Failure(MakeError(SaveErrors::StorageQuotaExceeded));
        auto write = Repack(archive, target.address.slot, target.generation, display, state_->policy, *state_->host);
        if (write.HasError())
            return Result<SaveStorageWrite>::Failure(write.ErrorValue());
        auto prepared = std::move(write).Value();
        if (destination != catalog.records.end()) {
            prepared.metadata.publication.kind = destination->entry.publication.kind;
            catalog.retired.emplace_back(destination->entry, false);
            destination->entry = prepared.metadata;
        } else {
            catalog.records.emplace_back(prepared.metadata, false);
        }
        return Result<SaveStorageWrite>::Success(std::move(prepared));
    }

    /** @copydoc SaveSlotLifecycle::PublishPrepared */
    Result<void> SaveSlotLifecycle::PublishPrepared(const std::optional<SaveStorageWrite> &prepared,
                                                    const std::span<const std::byte> catalog, const CancellationToken &cancellation) const {
        if (prepared) {
            const auto generation = prepared->metadata.publication.generation;
            if (auto absent = state_->storage.VerifyLifecycleGenerationAbsent(generation); absent.HasError())
                return Result<void>::Failure(absent.ErrorValue());
            const auto journal = EncodeJournal({generation, ComputeSha256(*prepared->archive.bytes)}, state_->policy.destination.name);
            if (auto journaled = state_->storage.ReplaceLifecycleJournal(journal); journaled.HasError())
                return Result<void>::Failure(journaled.ErrorValue());
            if (auto written = state_->storage.WriteLifecycleGeneration(generation, *prepared->archive.bytes); written.HasError())
                return Result<void>::Failure(written.ErrorValue());
        }
        if (auto cancelled = CheckCancellation(cancellation); cancelled.HasError())
            return Result<void>::Failure(cancelled.ErrorValue());
        if (auto published = state_->storage.ReplaceLifecycleCatalog(catalog); published.HasError())
            return Result<void>::Failure(std::move(published).ErrorValue());
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
