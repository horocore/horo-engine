#include "SaveMigrationInternal.h"

#include <algorithm>
#include <format>
#include <new>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc SaveMigrationRecordContext::Fail */
    Error SaveMigrationRecordContext::Fail(Error cause, const std::string_view field) const {
        const std::string boundedField = SaveMigrationDetail::IsCanonicalMigrationIdentity(field) ? std::string{field} : "<invalid-field>";
        return WithCause(SaveMigrationDetail::MigrationError(SaveErrors::MigrationStepFailed,
                                                             std::format("step={} schema={}->{} participant={} record={} field={}",
                                                                         step.value, from.Value(), to.Value(), participant.Value(),
                                                                         record.ToString(), boundedField)),
                         std::move(cause));
    }

    /** @copydoc RetainVerifiedSaveRecords */
    Result<void> RetainVerifiedSaveRecords(const ValidatedSaveArchive &archive, const SaveCompatibilityPolicy &policy,
                                           SaveMigrationSource &source, const SaveMigrationLimits &limits) {
        using namespace SaveMigrationDetail;
        if (!ValidLimits(limits) || !ValidParticipantPolicy(policy) ||
            source.archiveFormatVersion != archive.Preamble().archiveFormatVersion ||
            source.saveSchemaVersion != archive.Manifest().saveSchemaVersion ||
            source.productCompatibility != archive.Header().productCompatibility ||
            source.participants.size() != archive.Manifest().participants.size())
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationSourceUnsupported, "Detached migration state does not match the verified archive."));
        try {
            SaveMigrationSource candidate = source;
            std::uint64_t decodedBytes = 0;
            const auto entries = archive.Directory().Entries();
            for (const SaveManifestParticipant &manifest : archive.Manifest().participants) {
                auto participant =
                    std::ranges::lower_bound(candidate.participants, manifest.participant, {}, &SaveMigrationParticipantState::participant);
                if (participant == candidate.participants.end() || participant->participant != manifest.participant ||
                    participant->schemaVersion != manifest.schemaVersion || participant->required != manifest.required ||
                    !participant->records.empty())
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationSourceUnsupported,
                                       "Detached participant does not match the verified manifest: " + manifest.participant.Value()));
                const SaveParticipantCompatibility *support = FindPolicyParticipant(policy, manifest.participant);
                if (support == nullptr ||
                    (!support->versions.direct.Contains(manifest.schemaVersion) &&
                     (!support->versions.migrationSource || !support->versions.migrationSource->Contains(manifest.schemaVersion))))
                    continue;
                if (manifest.chunks.size() > limits.maximumRecordsPerParticipant)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded,
                                       "Participant record count exceeds the migration limit: " + manifest.participant.Value()));
                participant->records.reserve(manifest.chunks.size());
                for (const SaveRecordId recordId : manifest.chunks) {
                    const auto entry = std::ranges::lower_bound(entries, recordId, {}, &SaveChunkDirectoryEntry::record);
                    if (entry == entries.end() || entry->record != recordId || entry->owner != manifest.participant)
                        return Result<void>::Failure(MigrationError(SaveErrors::MigrationSourceUnsupported,
                                                                    "Verified record directory does not match the manifest."));
                    if (entry->decodedByteLength > limits.maximumParticipantPayloadBytes ||
                        entry->decodedByteLength > limits.maximumTotalPayloadBytes - decodedBytes)
                        return Result<void>::Failure(
                            MigrationError(SaveErrors::MigrationLimitExceeded, "Verified record bytes exceed the migration budget."));
                    decodedBytes += entry->decodedByteLength;
                    auto selected = archive.SelectChunk(recordId);
                    if (selected.HasError())
                        return Result<void>::Failure(selected.ErrorValue());
                    if (!selected.Value())
                        return Result<void>::Failure(MigrationError(SaveErrors::MigrationSourceUnsupported,
                                                                    "Verified record disappeared during migration staging."));
                    participant->records.push_back({.record = recordId,
                                                    .schemaVersion = manifest.schemaVersion,
                                                    .sourceParticipant = manifest.participant,
                                                    .sourceRecord = recordId,
                                                    .sourceSchemaVersion = manifest.schemaVersion,
                                                    .payload = std::move(selected).Value().value()});
                }
            }
            if (const auto validation = ValidateState(candidate, limits); validation.HasError())
                return validation;
            source = std::move(candidate);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    /** @copydoc RetainUnknownSaveData */
    Result<void> RetainUnknownSaveData(const ValidatedSaveArchive &archive, const SaveCompatibilityPolicy &policy,
                                       SaveMigrationSource &source, const std::uint64_t maximumPreservedBytes) {
        auto report = archive.InspectUnknownData(policy, maximumPreservedBytes);
        if (report.HasError())
            return Result<void>::Failure(report.ErrorValue());
        auto retained = std::move(report).Value();
        if (source.archiveFormatVersion != archive.Preamble().archiveFormatVersion ||
            source.saveSchemaVersion != archive.Manifest().saveSchemaVersion ||
            source.productCompatibility != archive.Header().productCompatibility ||
            source.participants.size() != archive.Manifest().participants.size())
            return Result<void>::Failure(
                SaveMigrationDetail::MigrationError(SaveErrors::MigrationSourceUnsupported,
                                                    "Detached migration versions do not match the verified source archive."));
        try {
            SaveMigrationSource candidate = source;
            for (const SaveManifestParticipant &manifestEntry : archive.Manifest().participants) {
                const auto found = std::ranges::lower_bound(candidate.participants, manifestEntry.participant, {},
                                                            &SaveMigrationParticipantState::participant);
                if (found == candidate.participants.end() || found->participant != manifestEntry.participant ||
                    found->schemaVersion != manifestEntry.schemaVersion || found->required != manifestEntry.required)
                    return Result<void>::Failure(
                        SaveMigrationDetail::MigrationError(SaveErrors::MigrationSourceUnsupported,
                                                            "Detached migration participant does not match verified manifest: " +
                                                                manifestEntry.participant.Value()));
            }
            for (PreservedSaveChunk &chunk : retained.preserved) {
                const auto found =
                    std::ranges::lower_bound(candidate.participants, chunk.entry.owner, {}, &SaveMigrationParticipantState::participant);
                found->preservedChunks.push_back(std::move(chunk));
            }
            if (const auto valid = SaveMigrationDetail::ValidateState(candidate, {}); valid.HasError())
                return valid;
            source = std::move(candidate);
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationId> SaveMigrationId::Parse(const std::string_view text) {
        if (!SaveMigrationDetail::IsCanonicalMigrationIdentity(text))
            return Result<SaveMigrationId>::Failure(
                SaveMigrationDetail::MigrationError(SaveErrors::MigrationDefinitionInvalid, "Save migration identity is not canonical."));
        try {
            return Result<SaveMigrationId>::Success({.value = std::string{text}});
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationId>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    bool SaveMigrationId::IsValid() const noexcept {
        return SaveMigrationDetail::IsCanonicalMigrationIdentity(value);
    }

    bool SaveMigrationPlan::IsNoOp() const noexcept {
        return definitions.empty() && sourceArchiveFormat == targetArchiveFormat && sourceSaveSchema == targetSaveSchema &&
               sourceProductCompatibility == targetProductCompatibility && !participantRequirementChanges;
    }
}  // namespace Horo::Runtime
