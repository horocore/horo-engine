#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveCloudRevisionMetadataInternal.h"

#include <algorithm>
#include <memory>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Rejects inconsistent publication identity before interpreting direct parent edges. */
        bool ValidCloudPublication(const SaveCloudPublicationHead &head) noexcept {
            return head.slot.IsValid() && head.generation.IsValid() &&
                   (!head.parent || (head.parent->IsValid() && head.parent != head.generation));
        }

        /** @brief Compares verified live heads without consulting timestamps or display data. */
        SaveCloudHeadRelation ComparePublications(const SaveCloudPublicationHead &local,
                                                  const SaveCloudPublicationHead &remote) noexcept {
            using enum SaveCloudHeadRelation;
            if (!ValidCloudPublication(local) || !ValidCloudPublication(remote) || local.slot != remote.slot)
                return IntegrityMismatch;
            if (local.generation == remote.generation)
                return local.archive == remote.archive && local.state == remote.state ? SamePublication : IntegrityMismatch;
            if (local.archive == remote.archive)
                return IntegrityMismatch;
            const bool localAhead = local.parent == remote.generation;
            const bool remoteAhead = remote.parent == local.generation;
            if (localAhead && remoteAhead)
                return IntegrityMismatch;
            if (localAhead)
                return LocalAhead;
            if (remoteAhead)
                return RemoteAhead;
            if (local.state == remote.state)
                return EquivalentState;
            return local.parent && local.parent == remote.parent ? Diverged : Unproven;
        }

        /** @brief Interprets an explicit tombstone only for a matching artifact or a proven edit of it. */
        SaveCloudHeadRelation CompareDeletion(const SaveCloudDeletionHead &deleted, const SaveCloudPublicationHead &live,
                                              const SaveCloudHeadRelation exactDeletion) noexcept {
            using enum SaveCloudHeadRelation;
            if (!deleted.slot.IsValid() || !deleted.removed.IsValid() || !ValidCloudPublication(live) || deleted.slot != live.slot)
                return IntegrityMismatch;
            if (deleted.removed == live.generation)
                return deleted.archive == live.archive ? exactDeletion : IntegrityMismatch;
            if (deleted.archive == live.archive)
                return IntegrityMismatch;
            return live.parent == deleted.removed ? DeleteEdit : Unproven;
        }
        [[nodiscard]] bool ValidLimits(const SaveCloudRevisionMetadataLimits &limits) noexcept {
            return limits.maximumRecords != 0 && limits.maximumRecords <= 4'096 && limits.maximumObjectKeyBytes != 0 &&
                   limits.maximumObjectKeyBytes <= 512 && limits.maximumRevisionBytes != 0 && limits.maximumRevisionBytes <= 512;
        }

        [[nodiscard]] bool KnownState(const SaveCloudGenerationState state) noexcept {
            using enum SaveCloudGenerationState;
            switch (state) {
                case Unknown:
                case Clean:
                case Dirty:
                case Uploading:
                case Downloading:
                case Conflicted:
                case Deleted:
                case Failed:
                    return true;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateRecord(const SaveCloudGenerationRecord &record, const SaveCloudRevisionMetadataLimits &limits) {
            using enum SaveCloudGenerationState;
            if (!record.slot.IsValid() || !record.generation.IsValid() || !KnownState(record.state) ||
                (record.lastConfirmed && !record.lastConfirmed->IsValid()))
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
            if (record.object) {
                if (record.object->key.empty() || (record.object->revision && record.object->revision->empty()))
                    return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
                if (record.object->key.size() > limits.maximumObjectKeyBytes ||
                    (record.object->revision && record.object->revision->size() > limits.maximumRevisionBytes))
                    return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataLimitExceeded));
            }
            if ((record.state == Clean || record.state == Downloading || record.state == Conflicted) &&
                (!record.object || !record.object->revision))
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] bool Matches(const SaveCloudGenerationRecord &record, const SaveSlotCatalogEntry &entry) noexcept {
            return record.slot == entry.publication.slot && record.generation == entry.publication.generation &&
                   record.archive == entry.publication.archiveContent;
        }

        [[nodiscard]] bool StructurallyValidPrevious(const SaveCloudRevisionMetadata &previous,
                                                     const SaveCloudRevisionMetadataLimits &limits) {
            if (previous.schemaVersion != SaveCloudRevisionMetadataSchemaVersion || !previous.scope.IsValid() || previous.revision == 0 ||
                previous.indexRevision == 0 || previous.records.size() > limits.maximumRecords)
                return false;
            for (std::size_t i = 0; i < previous.records.size(); ++i) {
                if (ValidateRecord(previous.records[i], limits).HasError() ||
                    (i != 0 && !(previous.records[i - 1].slot < previous.records[i].slot)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] const SaveCloudGenerationRecord *FindPrevious(const SaveCloudRevisionMetadata &previous,
                                                                    const SaveGameSlotId &slot) noexcept {
            const auto found = std::ranges::lower_bound(previous.records, slot, {}, &SaveCloudGenerationRecord::slot);
            return found != previous.records.end() && found->slot == slot ? std::to_address(found) : nullptr;
        }

        /** @brief Builds exact selected generation evidence for either live publication or restart. */
        Result<SaveCloudRevisionMetadata> PairMetadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &scope,
                                                      const SaveCloudRevisionMetadata *previous, const std::uint64_t revision,
                                                      const bool resetInFlight) {
            SaveCloudRevisionMetadata candidate{.revision = revision, .indexRevision = index.revision, .scope = scope};
            try {
                candidate.records.reserve(index.entries.size());
                using enum SaveCloudGenerationState;
                for (const auto &entry : index.entries) {
                    const auto slot = entry.publication.slot;
                    const auto *found = previous ? FindPrevious(*previous, slot) : nullptr;
                    if (found && Matches(*found, entry)) {
                        candidate.records.push_back(*found);
                        auto &record = candidate.records.back();
                        if (resetInFlight && (record.state == Uploading || record.state == Downloading))
                            record.state = Unknown;
                    } else {
                        candidate.records.push_back(
                            {.slot = slot, .generation = entry.publication.generation, .archive = entry.publication.archiveContent});
                    }
                }
            } catch (const std::bad_alloc &) {
                return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
            }
            return Result<SaveCloudRevisionMetadata>::Success(std::move(candidate));
        }
    }  // namespace

    /** @copydoc CompareSaveCloudHeads */
    SaveCloudHeadRelation CompareSaveCloudHeads(const SaveCloudHead &local, const SaveCloudHead &remote) noexcept {
        using enum SaveCloudHeadRelation;
        const auto *localLive = std::get_if<SaveCloudPublicationHead>(&local);
        const auto *remoteLive = std::get_if<SaveCloudPublicationHead>(&remote);
        if (localLive && remoteLive)
            return ComparePublications(*localLive, *remoteLive);

        const auto *localDeleted = std::get_if<SaveCloudDeletionHead>(&local);
        const auto *remoteDeleted = std::get_if<SaveCloudDeletionHead>(&remote);
        if (localDeleted && remoteLive)
            return CompareDeletion(*localDeleted, *remoteLive, LocalDeleted);
        if (remoteDeleted && localLive)
            return CompareDeletion(*remoteDeleted, *localLive, RemoteDeleted);
        if (!localDeleted || !remoteDeleted)
            return Unproven;
        if (!localDeleted->slot.IsValid() || !localDeleted->removed.IsValid() || !remoteDeleted->removed.IsValid() ||
            localDeleted->slot != remoteDeleted->slot)
            return IntegrityMismatch;
        if (localDeleted->removed != remoteDeleted->removed)
            return localDeleted->archive == remoteDeleted->archive ? IntegrityMismatch : Unproven;
        return localDeleted->archive == remoteDeleted->archive ? BothDeleted : IntegrityMismatch;
    }

    /** @copydoc SaveCloudRevisionMetadataDetail::RebindIndex */
    Result<SaveCloudRevisionMetadata> SaveCloudRevisionMetadataDetail::RebindIndex(const SaveSlotIndex &index,
                                                                                  const SaveCloudRevisionMetadata &previous) {
        const SaveCloudRevisionMetadataLimits limits;
        if (index.entries.size() > limits.maximumRecords || ValidateSaveSlotIndex(index).HasError() ||
            !StructurallyValidPrevious(previous, limits) || previous.indexRevision >= index.revision)
            return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        return PairMetadata(index, previous.scope, &previous, previous.revision, false);
    }

    /** @copydoc SaveCloudMetadataScope::IsValid */
    bool SaveCloudMetadataScope::IsValid() const noexcept {
        return localNamespace.IsValid() && provider.IsValid() && account.IsValid();
    }

    /** @copydoc ValidateSaveCloudRevisionMetadata */
    Result<void> ValidateSaveCloudRevisionMetadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &expectedScope,
                                                   const SaveCloudRevisionMetadata &metadata,
                                                   const SaveCloudRevisionMetadataLimits &limits) {
        if (!ValidLimits(limits) || metadata.records.size() > limits.maximumRecords || index.entries.size() > limits.maximumRecords)
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataLimitExceeded));
        if (ValidateSaveSlotIndex(index).HasError() || metadata.schemaVersion != SaveCloudRevisionMetadataSchemaVersion ||
            metadata.revision == 0 || !expectedScope.IsValid() || !metadata.scope.IsValid() || metadata.scope != expectedScope)
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        if (metadata.indexRevision != index.revision)
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        if (metadata.records.size() != index.entries.size())
            return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        for (std::size_t i = 0; i < metadata.records.size(); ++i) {
            if (auto valid = ValidateRecord(metadata.records[i], limits); valid.HasError())
                return valid;
            if (!Matches(metadata.records[i], index.entries[i]))
                return Result<void>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        }
        return Result<void>::Success();
    }

    struct SaveCloudRevisionSnapshot::Data final {
        Data(SaveSlotIndex indexValue, SaveCloudRevisionMetadata metadataValue)
            : index(std::move(indexValue)), metadata(std::move(metadataValue)) {}

        SaveSlotIndex index;
        SaveCloudRevisionMetadata metadata;
    };

    SaveCloudRevisionSnapshot::SaveCloudRevisionSnapshot(std::shared_ptr<const Data> data) noexcept : data_(std::move(data)) {}

    /** @copydoc SaveCloudRevisionSnapshot::Create */
    Result<SaveCloudRevisionSnapshot> SaveCloudRevisionSnapshot::Create(SaveSlotIndex index, const SaveCloudMetadataScope &expectedScope,
                                                                        SaveCloudRevisionMetadata metadata,
                                                                        const SaveCloudRevisionMetadataLimits limits) {
        if (auto valid = ValidateSaveCloudRevisionMetadata(index, expectedScope, metadata, limits); valid.HasError())
            return Result<SaveCloudRevisionSnapshot>::Failure(valid.ErrorValue());
        try {
            return Result<SaveCloudRevisionSnapshot>::Success(
                SaveCloudRevisionSnapshot{std::make_shared<const Data>(std::move(index), std::move(metadata))});
        } catch (const std::bad_alloc &) {
            return Result<SaveCloudRevisionSnapshot>::Failure(MakeError(SaveErrors::CloudMetadataAllocationFailed));
        }
    }

    /** @copydoc SaveCloudRevisionSnapshot::Index */
    const SaveSlotIndex &SaveCloudRevisionSnapshot::Index() const noexcept {
        return data_->index;
    }

    /** @copydoc SaveCloudRevisionSnapshot::Metadata */
    const SaveCloudRevisionMetadata &SaveCloudRevisionSnapshot::Metadata() const noexcept {
        return data_->metadata;
    }

    /** @copydoc ReconcileSaveCloudRevisionMetadata */
    Result<SaveCloudRevisionMetadata> ReconcileSaveCloudRevisionMetadata(const SaveSlotIndex &index, const SaveCloudMetadataScope &scope,
                                                                         const std::optional<SaveCloudRevisionMetadata> &previous,
                                                                         const std::uint64_t nextRevision,
                                                                         const SaveCloudRevisionMetadataLimits &limits) {
        if (!ValidLimits(limits) || index.entries.size() > limits.maximumRecords)
            return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataLimitExceeded));
        if (ValidateSaveSlotIndex(index).HasError() || !scope.IsValid() || nextRevision == 0)
            return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataInvalid));
        const bool validPrevious = previous && previous->scope == scope && StructurallyValidPrevious(*previous, limits);
        if (validPrevious && nextRevision <= previous->revision)
            return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        if (validPrevious && previous->indexRevision > index.revision)
            return Result<SaveCloudRevisionMetadata>::Failure(MakeError(SaveErrors::CloudMetadataStale));
        return PairMetadata(index, scope, validPrevious ? &*previous : nullptr, nextRevision, true);
    }
}  // namespace Horo::Runtime
