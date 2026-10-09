#include "Horo/Runtime/Save/SaveThumbnailArchive.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveThumbnailCodec.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc SaveThumbnailArchiveOwner */
    SaveParticipantId SaveThumbnailArchiveOwner() {
        return SaveParticipantId::Parse("horo.save.presentation.v1").Value();
    }

    /** @copydoc SaveThumbnailMetadataRecord */
    SaveRecordId SaveThumbnailMetadataRecord() {
        return SaveRecordId::Parse("b27b6271-528b-4a13-8cc1-0d3d8c6e0001").Value();
    }

    /** @copydoc SaveThumbnailImageRecord */
    SaveRecordId SaveThumbnailImageRecord() {
        return SaveRecordId::Parse("b27b6271-528b-4a13-8cc1-0d3d8c6e0002").Value();
    }

    namespace {
        /** @brief Rejects reserved owner/record collisions before copying logical chunks. */
        [[nodiscard]] bool HasReservationCollision(const SavePresentationArchiveInput &input) {
            const auto owner = SaveThumbnailArchiveOwner();
            const auto metadata = SaveThumbnailMetadataRecord();
            const auto image = SaveThumbnailImageRecord();
            return std::ranges::any_of(input.manifest.participants, [&](const SaveManifestParticipant &participant) {
                return participant.participant == owner || std::ranges::find(participant.chunks, metadata) != participant.chunks.end() ||
                       std::ranges::find(participant.chunks, image) != participant.chunks.end();
            }) || std::ranges::any_of(input.chunks, [&](const PreservedSaveChunk &chunk) {
                return chunk.entry.owner == owner || chunk.entry.record == metadata || chunk.entry.record == image;
            });
        }

        /** @brief Creates existing raw chunk framing and decoded integrity for presentation bytes. */
        [[nodiscard]] PreservedSaveChunk Chunk(const SaveRecordId record, std::vector<std::byte> bytes) {
            SaveChunkDirectoryEntry entry{.record = record,
                                          .owner = SaveThumbnailArchiveOwner(),
                                          .storedByteLength = bytes.size(),
                                          .decodedByteLength = bytes.size(),
                                          .decodedHash = ComputeSha256(bytes)};
            return {std::move(entry), std::move(bytes)};
        }

        /** @brief Checks archive/candidate identity and existing finite manifest admission before allocation. */
        [[nodiscard]] Result<void> ValidateInput(const SavePresentationArchiveInput &input) {
            if (const auto valid = ValidateSaveGameManifest(input.manifest, input.limits.metadata); valid.HasError())
                return valid;
            if (input.chunks.size() > input.limits.maximumEntries || HasReservationCollision(input))
                return Result<void>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
            if (input.header.slot != input.publication.slot || input.header.slotGeneration != input.publication.generation ||
                input.header.baseScene != input.publication.baseScene ||
                input.header.productCompatibility != input.publication.productCompatibility ||
                input.manifest.saveSchemaVersion != input.publication.saveSchema ||
                input.manifest.canonicalState != input.publication.canonicalState)
                return Result<void>::Failure(MakeError(SaveErrors::ThumbnailStale));
            return Result<void>::Success();
        }

        /** @brief Preserves the logical archive when only an optional presentation attachment exceeds limits. */
        [[nodiscard]] auto FinalizePresentationArchive(const SavePresentationArchiveInput &input, const SaveGameManifest &manifest,
                                                       const std::vector<PreservedSaveChunk> &chunks,
                                                       SaveSlotPublicationMetadata &publication) {
            auto limits = input.limits;
            // A tighter archive/storage ceiling also bounds its payload; reader admission requires consistent ceilings.
            limits.maximumStoredPayloadBytes =
                std::min(limits.maximumStoredPayloadBytes, static_cast<std::uint64_t>(limits.maximumArchiveBytes));
            limits.chunks.maximumPayloadBytes = std::min(limits.chunks.maximumPayloadBytes, limits.maximumStoredPayloadBytes);
            limits.chunks.maximumStoredChunkBytes = std::min(limits.chunks.maximumStoredChunkBytes, limits.maximumStoredPayloadBytes);
            limits.chunks.maximumEntries = std::min(limits.chunks.maximumEntries, limits.maximumEntries);
            limits.metadata.maximumTotalChunks = std::min(limits.metadata.maximumTotalChunks, limits.maximumEntries);
            auto finalized = SaveArchiveContainerWriter::Write(input.header, manifest, chunks, input.version, limits);
            if (finalized.HasError() && input.capture.artifact && input.capture.request->policy == SaveThumbnailPolicy::Optional) {
                finalized = SaveArchiveContainerWriter::Write(input.header, input.manifest, input.chunks, input.version, limits);
                publication.thumbnail.reset();
            }
            return finalized;
        }

        /** @brief Admits supported image dimensions and encoded byte ceilings before archive payload selection. */
        [[nodiscard]] bool ValidImageLimits(const SaveThumbnailLimits &limits) noexcept {
            return limits.maximumDimension != 0 && limits.maximumDimension <= 4'096 && limits.maximumEncodedBytes != 0 &&
                   limits.maximumEncodedBytes <= (16U << 20U);
        }

        /** @brief Admits only exact optional schema-1 raw records before selecting their payloads. */
        [[nodiscard]] bool ValidRecords(const ValidatedSaveArchive &archive, const SaveManifestParticipant &owner,
                                        const SaveThumbnailLimits &limits) {
            const std::vector expected{SaveThumbnailMetadataRecord(), SaveThumbnailImageRecord()};
            if (owner.required || owner.schemaVersion.Value() != 1 || owner.chunks != expected || !ValidImageLimits(limits))
                return false;
            return std::ranges::all_of(archive.Directory().Entries(), [&](const SaveChunkDirectoryEntry &entry) {
                if (entry.owner != owner.participant)
                    return true;
                const auto bound = entry.record == expected.front() ? std::size_t{104} : limits.maximumEncodedBytes;
                return entry.codec == SaveChunkCodec::Raw && entry.storedByteLength != 0 && entry.storedByteLength <= bound;
            });
        }

        /** @brief Selects the admitted metadata and image records in order before decoding detached presentation. */
        [[nodiscard]] Result<std::shared_ptr<const SaveThumbnailArtifact>> DecodeArchiveThumbnail(
            const ValidatedSaveArchive &archive, const SaveSlotPublicationMetadata &publication, const SaveThumbnailLimits &limits) {
            using Return = Result<std::shared_ptr<const SaveThumbnailArtifact>>;
            auto metadata = archive.SelectChunk(SaveThumbnailMetadataRecord());
            if (metadata.HasError())
                return Return::Failure(metadata.ErrorValue());
            auto image = archive.SelectChunk(SaveThumbnailImageRecord());
            if (image.HasError())
                return Return::Failure(image.ErrorValue());
            if (!metadata.Value() || !image.Value())
                return Return::Failure(MakeError(SaveErrors::ThumbnailInvalid));
            return SaveThumbnailDetail::Decode(*metadata.Value(), std::move(image).Value().value(), publication, limits);
        }
    }  // namespace

    /** @copydoc PrepareSavePresentationWrite */
    Result<SaveStorageWrite> PrepareSavePresentationWrite(const SavePresentationArchiveInput &input) {
        using Return = Result<SaveStorageWrite>;
        try {
            if (const auto valid = ValidateInput(input); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            auto publication = input.publication;
            publication.thumbnail = input.capture.artifact ? std::optional{input.capture.artifact->Request().thumbnail} : std::nullopt;
            if (const auto valid = ValidateSaveThumbnailPublication(publication, input.capture); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            auto manifest = input.manifest;
            auto chunks = input.chunks;
            if (input.capture.artifact) {
                const auto &artifact = *input.capture.artifact;
                manifest.participants.emplace_back(SaveThumbnailArchiveOwner(), ParticipantSchemaVersion::Create(1).Value(), false,
                                                   std::vector<SaveRecordId>{SaveThumbnailMetadataRecord(), SaveThumbnailImageRecord()});
                chunks.push_back(Chunk(SaveThumbnailMetadataRecord(), SaveThumbnailDetail::Encode(artifact)));
                const auto bytes = artifact.Bytes();
                chunks.push_back(Chunk(SaveThumbnailImageRecord(), {bytes.begin(), bytes.end()}));
                std::ranges::sort(manifest.participants, {}, &SaveManifestParticipant::participant);
                std::ranges::sort(chunks, {}, [](const PreservedSaveChunk &chunk) {
                    return chunk.entry.record;
                });
            }
            auto finalized = FinalizePresentationArchive(input, manifest, chunks, publication);
            if (finalized.HasError())
                return Return::Failure(finalized.ErrorValue());
            publication.archiveContent = finalized.Value().Summary().integrity.archiveContent;
            auto display = input.display;
            if (ValidateSaveSlotDisplayMetadata(display).HasError())
                display = {};
            return Return::Success({.metadata = {.publication = std::move(publication), .display = std::move(display)},
                                    .archive = finalized.Value().Archive()});
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SaveErrors::ThumbnailAllocationFailed));
        }
    }

    /** @copydoc ReadSaveThumbnail */
    Result<std::optional<std::shared_ptr<const SaveThumbnailArtifact>>> ReadSaveThumbnail(const ValidatedSaveArchive &archive,
                                                                                          const SaveSlotPublicationMetadata &publication,
                                                                                          const SaveThumbnailLimits &limits) {
        using Return = Result<std::optional<std::shared_ptr<const SaveThumbnailArtifact>>>;
        try {
            if (archive.Header().slot != publication.slot || archive.Header().slotGeneration != publication.generation ||
                archive.Integrity().archiveContent != publication.archiveContent)
                return Return::Failure(MakeError(SaveErrors::ThumbnailStale));
            if (const auto valid = ValidateSaveSlotPublicationMetadata(publication); valid.HasError())
                return Return::Failure(valid.ErrorValue());
            const auto &participants = archive.Manifest().participants;
            const auto owner = std::ranges::find(participants, SaveThumbnailArchiveOwner(), &SaveManifestParticipant::participant);
            if (owner == participants.end())
                return publication.thumbnail ? Return::Failure(MakeError(SaveErrors::ThumbnailStale)) : Return::Success(std::nullopt);
            if (!ValidRecords(archive, *owner, limits))
                return Return::Failure(MakeError(SaveErrors::ThumbnailInvalid));
            auto artifact = DecodeArchiveThumbnail(archive, publication, limits);
            if (artifact.HasError())
                return Return::Failure(artifact.ErrorValue());
            return Return::Success(std::move(artifact).Value());
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SaveErrors::ThumbnailAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
