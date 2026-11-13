#include "EditorPlaySaveHostArchive.h"

#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

#include <algorithm>
#include <array>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Copies only the actual admitted immutable segments under one aggregate payload bound. */
        Result<PreservedSaveChunk> CopyRecord(const OwnedCanonicalSnapshot &record, std::size_t &remaining) {
            using Outcome = Result<PreservedSaveChunk>;
            if (record.ByteLength() > remaining)
                return Outcome::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
            std::vector<std::byte> bytes;
            bytes.reserve(static_cast<std::size_t>(record.ByteLength()));
            for (std::size_t index = 0; index < record.SegmentCount(); ++index) {
                const auto segment = record.Segment(index);
                if (segment.size() > record.ByteLength() - bytes.size())
                    return Outcome::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
                bytes.insert(bytes.end(), segment.begin(), segment.end());
            }
            if (bytes.size() != record.ByteLength())
                return Outcome::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            remaining -= bytes.size();
            SaveChunkDirectoryEntry entry{.record = record.Record().record,
                                          .owner = record.Record().participant,
                                          .storedByteLength = bytes.size(),
                                          .decodedByteLength = bytes.size(),
                                          .decodedHash = ComputeSha256(bytes)};
            return Outcome::Success({std::move(entry), std::move(bytes)});
        }

        /** @brief Carries actual manifest ownership and immutable canonical data into the existing container writer. */
        struct ArchiveParts final {
            SaveGameManifest manifest;
            std::vector<PreservedSaveChunk> chunks;
        };

        /** @brief Preserves optional omission and canonical participant/schema identity from the real capture. */
        Result<ArchiveParts> CaptureParts(const RuntimeSaveSnapshot &snapshot) {
            using Outcome = Result<ArchiveParts>;
            if (!snapshot.IsValid() || snapshot.Records().size() > MaximumSaveParticipantCount)
                return Outcome::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            ArchiveParts parts{.manifest = {.saveSchemaVersion = SaveSchemaVersion::Create(1).Value()}};
            for (const auto &participant : snapshot.Participants())
                if (participant.disposition == CanonicalCaptureDisposition::Captured)
                    parts.manifest.participants.push_back(
                        {participant.participant, participant.schemaVersion, participant.required, participant.records});
            std::size_t remaining = PlaySaveMaximumPayloadBytes;
            parts.chunks.reserve(snapshot.Records().size());
            for (const auto &record : snapshot.Records()) {
                auto copied = CopyRecord(record, remaining);
                if (copied.HasError())
                    return Outcome::Failure(copied.ErrorValue());
                parts.chunks.push_back(std::move(copied).Value());
            }
            std::ranges::sort(parts.chunks, {}, [](const auto &chunk) {
                return chunk.entry.record;
            });
            return Outcome::Success(std::move(parts));
        }

        /** @brief Domain-separates canonical state from publication identity, time, storage and display labels. */
        Result<void> HashHeader(CanonicalValueWriter &writer, const SaveArchiveHeader &header) {
            const std::array writes{writer.WriteUtf8("horo.play.save.logical.v1"),
                                    writer.WriteBytes(std::as_bytes(std::span{header.project.Bytes()})),
                                    writer.WriteBytes(std::as_bytes(std::span{header.world.Bytes()})),
                                    writer.WriteBytes(std::as_bytes(std::span{header.baseScene.Bytes()}))};
            for (const auto &write : writes)
                if (write.HasError())
                    return Result<void>::Failure(write.ErrorValue());
            return Result<void>::Success();
        }

        /** @brief Hashes a complete owner/schema/record tuple and the actual canonical record bytes. */
        Result<void> HashParticipant(CanonicalValueWriter &writer, const SaveManifestParticipant &participant,
                                     std::span<const PreservedSaveChunk> chunks) {
            const std::array writes{writer.WriteUtf8(participant.participant.Value()),
                                    writer.WriteUInt32(participant.schemaVersion.Value()), writer.WriteBool(participant.required),
                                    writer.WriteUInt32(static_cast<std::uint32_t>(participant.chunks.size()))};
            for (const auto &write : writes)
                if (write.HasError())
                    return Result<void>::Failure(write.ErrorValue());
            for (const auto record : participant.chunks) {
                const auto found = std::ranges::lower_bound(chunks, record, {}, [](const auto &chunk) {
                    return chunk.entry.record;
                });
                if (found == chunks.end() || found->entry.record != record || found->entry.owner != participant.participant)
                    return Result<void>::Failure(MakeError(SaveErrors::CaptureIncomplete));
                if (const auto identity = writer.WriteBytes(std::as_bytes(std::span{record.Bytes()})); identity.HasError())
                    return identity;
                if (const auto payload = writer.WriteBytes(found->storedBytes); payload.HasError())
                    return payload;
            }
            return Result<void>::Success();
        }

        /** @brief Uses one bounded canonical projection for both worker construction and post-read logical verification. */
        Result<CanonicalStateHash> LogicalHash(const SaveArchiveHeader &header, const ArchiveParts &parts) {
            using Outcome = Result<CanonicalStateHash>;
            CanonicalValueWriter writer({.maximumBytes = PlaySaveMaximumArchiveBytes});
            if (const auto prefix = HashHeader(writer, header); prefix.HasError())
                return Outcome::Failure(prefix.ErrorValue());
            for (const auto &participant : parts.manifest.participants)
                if (const auto encoded = HashParticipant(writer, participant, parts.chunks); encoded.HasError())
                    return Outcome::Failure(encoded.ErrorValue());
            const auto encoded = std::move(writer).Finalize();
            if (encoded.HasError())
                return Outcome::Failure(encoded.ErrorValue());
            return Outcome::Success(ComputeCanonicalStateHash(encoded.Value().Bytes()));
        }

        /** @brief Decodes actual independently integrity-verified chunks under an aggregate finite copy bound. */
        Result<ArchiveParts> ReadParts(const ValidatedSaveArchive &archive) {
            using Outcome = Result<ArchiveParts>;
            if (archive.Manifest().saveSchemaVersion.Value() != 1 || archive.Directory().Entries().size() > MaximumSaveParticipantCount)
                return Outcome::Failure(MakeError(SaveErrors::CaptureRecordInvalid));
            ArchiveParts parts{archive.Manifest(), {}};
            std::size_t remaining = PlaySaveMaximumPayloadBytes;
            for (const auto &entry : archive.Directory().Entries()) {
                if (entry.decodedByteLength > remaining)
                    return Outcome::Failure(MakeError(SaveErrors::CaptureBudgetExceeded));
                auto bytes = archive.SelectChunk(entry.record);
                if (bytes.HasError())
                    return Outcome::Failure(bytes.ErrorValue());
                if (!bytes.Value())
                    return Outcome::Failure(MakeError(SaveErrors::CaptureIncomplete));
                auto decoded = std::move(*std::move(bytes).Value());
                remaining -= decoded.size();
                parts.chunks.push_back({entry, std::move(decoded)});
            }
            return Outcome::Success(std::move(parts));
        }
    }  // namespace

    /** @copydoc PlaySaveArchiveLimits */
    Runtime::SaveArchiveReaderLimits PlaySaveArchiveLimits() noexcept {
        Runtime::SaveArchiveReaderLimits limits;
        limits.maximumArchiveBytes = PlaySaveMaximumArchiveBytes;
        limits.maximumStoredPayloadBytes = PlaySaveMaximumArchiveBytes;
        limits.maximumDecodedBytes = PlaySaveMaximumArchiveBytes;
        // The container directory includes header and manifest in addition to participant record chunks.
        limits.maximumEntries = Runtime::MaximumSaveParticipantCount + 2;
        limits.metadata.maximumParticipants = Runtime::MaximumSaveParticipantCount;
        limits.metadata.maximumTotalChunks = Runtime::MaximumSaveParticipantCount;
        limits.metadata.maximumChunksPerParticipant = Runtime::MaximumSaveParticipantCount;
        limits.metadata.maximumNestingDepth = limits.maximumNestingDepth;
        // Reader admission applies this nested cap to the whole container header count as well.
        limits.chunks.maximumEntries = limits.maximumEntries;
        limits.chunks.maximumPayloadBytes = PlaySaveMaximumArchiveBytes;
        limits.chunks.maximumStoredChunkBytes = PlaySaveMaximumPayloadBytes;
        limits.chunks.maximumDecodedChunkBytes = PlaySaveMaximumPayloadBytes;
        limits.chunks.maximumExpansionRatio = limits.maximumExpansionRatio;
        limits.maximumReadWorkBytes = 128U << 20U;
        return limits;
    }

    /** @copydoc BuildPlaySaveArchive */
    Result<Runtime::FinalizedSaveArchive> BuildPlaySaveArchive(const Runtime::RuntimeSaveSnapshot &snapshot,
                                                               const Runtime::SaveArchiveHeader &header) {
        using Outcome = Result<Runtime::FinalizedSaveArchive>;
        auto captured = CaptureParts(snapshot);
        if (captured.HasError())
            return Outcome::Failure(captured.ErrorValue());
        auto parts = std::move(captured).Value();
        const auto hash = LogicalHash(header, parts);
        if (hash.HasError())
            return Outcome::Failure(hash.ErrorValue());
        parts.manifest.canonicalState = hash.Value();
        return Runtime::SaveArchiveContainerWriter::Write(header, parts.manifest, parts.chunks,
                                                          Runtime::ArchiveFormatVersion::Create(1).Value(), PlaySaveArchiveLimits());
    }

    /** @copydoc ValidatePlaySaveArchiveState */
    Result<void> ValidatePlaySaveArchiveState(const Runtime::ValidatedSaveArchive &archive) {
        const auto parts = ReadParts(archive);
        if (parts.HasError())
            return Result<void>::Failure(parts.ErrorValue());
        const auto hash = LogicalHash(archive.Header(), parts.Value());
        if (hash.HasError())
            return Result<void>::Failure(hash.ErrorValue());
        if (hash.Value() != archive.Manifest().canonicalState)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::CanonicalStateHashMismatch));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
