#include "Horo/Runtime/Save/SaveCloudRevisionMetadata.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
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
            if ((record.state == SaveCloudGenerationState::Clean || record.state == SaveCloudGenerationState::Downloading ||
                 record.state == SaveCloudGenerationState::Conflicted) &&
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
            const auto found = std::lower_bound(previous.records.begin(), previous.records.end(), slot,
                                                [](const SaveCloudGenerationRecord &record, const SaveGameSlotId &value) {
                return record.slot < value;
            });
            return found != previous.records.end() && found->slot == slot ? &*found : nullptr;
        }
    }  // namespace

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
                SaveCloudRevisionSnapshot{std::make_shared<const Data>(Data{std::move(index), std::move(metadata)})});
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
        SaveCloudRevisionMetadata candidate{.schemaVersion = SaveCloudRevisionMetadataSchemaVersion,
                                            .revision = nextRevision,
                                            .indexRevision = index.revision,
                                            .scope = scope};
        try {
            candidate.records.reserve(index.entries.size());
            for (const auto &entry : index.entries) {
                const auto slot = entry.publication.slot;
                const auto *found = validPrevious ? FindPrevious(*previous, slot) : nullptr;
                if (found && Matches(*found, entry)) {
                    candidate.records.push_back(*found);
                    auto &record = candidate.records.back();
                    if (record.state == SaveCloudGenerationState::Uploading || record.state == SaveCloudGenerationState::Downloading)
                        record.state = SaveCloudGenerationState::Unknown;
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
}  // namespace Horo::Runtime
