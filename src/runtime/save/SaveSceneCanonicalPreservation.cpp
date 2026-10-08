#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSceneCanonicalState.h"
#include "SaveArchiveReaderInternal.h"

#include <algorithm>
#include <new>
#include <tuple>

namespace Horo::Runtime {
    /** @copydoc ValidatedSaveSceneCanonicalPreservation::ValidatedSaveSceneCanonicalPreservation */
    ValidatedSaveSceneCanonicalPreservation::ValidatedSaveSceneCanonicalPreservation(
        ValidatedSaveArchive archive, std::vector<SaveSceneCanonicalLayoutEntry> layout) noexcept
        : archive_(std::move(archive)), layout_(std::move(layout)) {}

    /** @copydoc ValidatedSaveSceneCanonicalPreservation::Charge */
    bool ValidatedSaveSceneCanonicalPreservation::Charge(const ValidatedSaveArchive &archive, const std::uint64_t bytes) noexcept {
        if (!archive.remainingReadWork_)
            return false;
        auto remaining = archive.remainingReadWork_->load(std::memory_order_relaxed);
        while (bytes <= remaining) {
            if (archive.remainingReadWork_->compare_exchange_weak(remaining, remaining - bytes, std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    /** @copydoc ValidatedSaveSceneCanonicalPreservation::Create */
    Result<ValidatedSaveSceneCanonicalPreservation> ValidatedSaveSceneCanonicalPreservation::Create(const ValidatedSaveArchive &archive,
                                                                                                    const std::uint64_t maximumBytes) {
        using Proof = ValidatedSaveSceneCanonicalPreservation;
        if (!archive.ownedArchive_ ||
            (archive.Preamble().archiveFormatVersion.Value() != 1 && archive.Preamble().archiveFormatVersion.Value() != 2) ||
            archive.Manifest().saveSchemaVersion.Value() != SaveSceneCanonicalSchemaVersion)
            return Result<Proof>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
        if (!Charge(archive, archive.Payload().size()))
            return Result<Proof>::Failure(SaveArchiveReaderDetail::ReadWorkError(0, "canonicalPreservation/hashWork"));
        auto layout = ValidateSaveSceneCanonicalArchive(archive, maximumBytes);
        if (layout.HasError())
            return Result<Proof>::Failure(layout.ErrorValue());
        try {
            return Result<Proof>::Success(Proof{archive, std::move(layout).Value()});
        } catch (const std::bad_alloc &) {
            return Result<Proof>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
        }
    }

    /** @copydoc ValidatedSaveSceneCanonicalPreservation::CollectOwner */
    Result<void> ValidatedSaveSceneCanonicalPreservation::CollectOwner(const SaveManifestParticipant &owner, const bool known,
                                                                       const std::uint64_t maximumPreservedBytes, std::uint64_t &retained,
                                                                       SaveUnknownDataReport &report) const {
        const auto directory = archive_.Directory().Entries();
        for (const auto record : owner.chunks) {
            const auto tag = std::ranges::lower_bound(layout_, record, {}, &SaveSceneCanonicalLayoutEntry::record);
            const bool opaque =
                tag != layout_.end() && tag->record == record && tag->representation == SaveSceneCanonicalRepresentation::Opaque;
            if (known && !opaque)
                continue;
            if (owner.required)
                return Result<void>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
            const auto entry = std::ranges::lower_bound(directory, record, {}, &SaveChunkDirectoryEntry::record);
            if (entry == directory.end() || entry->record != record || entry->owner != owner.participant)
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
            if (entry->storedByteLength > maximumPreservedBytes - retained)
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveMetadataLimitExceeded));
            if (!Charge(archive_, entry->storedByteLength))
                return Result<void>::Failure(SaveArchiveReaderDetail::ReadWorkError(entry->offset, "canonicalPreservation/copyWork"));
            if (!opaque) {
                auto decoded = archive_.SelectChunk(record);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                if (!decoded.Value())
                    return Result<void>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
            }
            const auto stored =
                archive_.Payload().subspan(static_cast<std::size_t>(entry->offset), static_cast<std::size_t>(entry->storedByteLength));
            report.preserved.push_back({*entry, {stored.begin(), stored.end()}});
            retained += entry->storedByteLength;
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidatedSaveSceneCanonicalPreservation::InspectUnknownData */
    Result<SaveUnknownDataReport> ValidatedSaveSceneCanonicalPreservation::InspectUnknownData(
        const SaveCompatibilityPolicy &policy, const std::uint64_t maximumPreservedBytes) const {
        if (!archive_.ownedArchive_ || layout_.empty() || !policy.droppableUnknownParticipants.empty() ||
            EvaluateSaveCompatibility(archive_.Preamble().archiveFormatVersion, archive_.Header(), archive_.Manifest(), policy)
                    .disposition != SaveCompatibilityDisposition::DirectRead)
            return Result<SaveUnknownDataReport>::Failure(MakeError(SaveErrors::MigrationSourceUnsupported));
        SaveUnknownDataReport report;
        std::uint64_t retained{};
        try {
            for (const auto &owner : archive_.Manifest().participants) {
                const auto installed =
                    std::ranges::lower_bound(policy.participants, owner.participant, {}, &SaveParticipantCompatibility::participant);
                const bool known = installed != policy.participants.end() && installed->participant == owner.participant &&
                                   installed->versions.direct.Contains(owner.schemaVersion);
                if (auto collected = CollectOwner(owner, known, maximumPreservedBytes, retained, report); collected.HasError())
                    return Result<SaveUnknownDataReport>::Failure(collected.ErrorValue());
            }
            std::ranges::sort(report.preserved, {}, [](const PreservedSaveChunk &chunk) {
                return chunk.entry.record;
            });
            return Result<SaveUnknownDataReport>::Success(std::move(report));
        } catch (const std::bad_alloc &) {
            return Result<SaveUnknownDataReport>::Failure(MakeError(SaveErrors::ArchiveAllocationFailed));
        }
    }

    /** @copydoc ValidatedSaveSceneCanonicalPreservation::VerifyRoundTrip */
    Result<void> ValidatedSaveSceneCanonicalPreservation::VerifyRoundTrip(const ValidatedSaveSceneCanonicalPreservation &destination,
                                                                          const SaveCompatibilityPolicy &policy,
                                                                          const std::uint64_t maximumPreservedBytes) const {
        auto expected = InspectUnknownData(policy, maximumPreservedBytes);
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        auto actual = destination.InspectUnknownData(policy, maximumPreservedBytes);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        const auto identity = [](const SaveChunkDirectoryEntry &entry) {
            return std::tie(entry.record, entry.owner, entry.codec, entry.storedByteLength, entry.decodedByteLength, entry.alignment,
                            entry.decodedHash);
        };
        for (const auto &chunk : expected.Value().preserved) {
            const auto candidate =
                std::ranges::lower_bound(actual.Value().preserved, chunk.entry.record, {}, [](const PreservedSaveChunk &value) {
                return value.entry.record;
            });
            if (candidate == actual.Value().preserved.end() || identity(chunk.entry) != identity(candidate->entry) ||
                chunk.storedBytes != candidate->storedBytes)
                return Result<void>::Failure(MakeError(SaveErrors::MigrationCandidateInvalid,
                                                       "Canonical preservation changed participant '" + chunk.entry.owner.Value() +
                                                           "', record " + chunk.entry.record.ToString() + "."));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
