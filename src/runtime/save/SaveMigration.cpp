#include "SaveMigrationInternal.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
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
