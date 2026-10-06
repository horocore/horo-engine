#include "Horo/Runtime/Save/SaveArchiveContainerWriter.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace Horo::Runtime {
    namespace {
        /** @brief Encodes fixed-width existing wire scalars into pre-admitted storage. */
        template <typename T> void Put(std::vector<std::byte> &bytes, const std::size_t offset, const T value) {
            for (std::size_t index = 0; index < sizeof(T); ++index)
                bytes[offset + index] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (index * 8U));
        }

        /** @brief Encodes one fixed wire entry; data offset is relative to the post-table region. */
        void Entry(std::vector<std::byte> &payload, const std::size_t index, const SaveArchiveEntryKind kind,
                   const SaveChunkDirectoryEntry &entry, const std::uint64_t relativeOffset) {
            const auto offset = SaveArchiveContainerHeaderByteLength + index * SaveArchiveContainerEntryByteLength;
            payload[offset] = static_cast<std::byte>(kind);
            Put(payload, offset + 2, static_cast<std::uint16_t>(entry.codec));
            if (kind == SaveArchiveEntryKind::Chunk) {
                std::ranges::transform(entry.record.Bytes(), payload.begin() + offset + 8, [](const std::uint8_t byte) {
                    return static_cast<std::byte>(byte);
                });
                const auto &owner = entry.owner.Value();
                Put(payload, offset + 24, static_cast<std::uint16_t>(owner.size()));
                std::ranges::transform(owner, payload.begin() + offset + 28, [](const char byte) {
                    return static_cast<std::byte>(static_cast<unsigned char>(byte));
                });
            }
            Put(payload, offset + 124, relativeOffset);
            Put(payload, offset + 132, entry.storedByteLength);
            Put(payload, offset + 140, entry.decodedByteLength);
            Put(payload, offset + 148, entry.alignment);
            std::ranges::transform(entry.decodedHash.bytes, payload.begin() + offset + 156, [](const std::uint8_t byte) {
                return static_cast<std::byte>(byte);
            });
        }

        /** @brief Solves compatible power-of-two chunk alignment constraints without modifying opaque storage.
         * @return Deterministic header-whitespace count, or failure when no contiguous layout exists.
         */
        Result<std::size_t> HeaderPadding(const std::span<const PreservedSaveChunk> chunks, std::uint64_t offset,
                                          const std::uint32_t maximumAlignment) {
            std::uint64_t modulus = 1;
            std::uint64_t padding = 0;
            for (const auto &chunk : chunks) {
                const auto alignment = chunk.entry.alignment;
                if (alignment == 0 || alignment > maximumAlignment || (alignment & (alignment - 1U)) != 0)
                    return Result<std::size_t>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                const auto required = (alignment - offset % alignment) % alignment;
                if (alignment >= modulus) {
                    if (required % modulus != padding)
                        return Result<std::size_t>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                    modulus = alignment;
                    padding = required;
                } else if (padding % alignment != required) {
                    return Result<std::size_t>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                }
                offset += chunk.storedBytes.size();
            }
            return Result<std::size_t>::Success(static_cast<std::size_t>(padding));
        }

        /** @brief Reserves raw digest work before hashing and rejects inconsistent caller-supplied stored evidence. */
        Result<std::uint64_t> ValidateRawChunks(const std::span<const PreservedSaveChunk> chunks, std::uint64_t remainingWork) {
            for (std::size_t index = 0; index < chunks.size(); ++index) {
                const auto &chunk = chunks[index];
                if (!chunk.entry.record.IsValid() || (index != 0 && chunks[index - 1].entry.record == chunk.entry.record))
                    return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
                if (chunk.entry.codec != SaveChunkCodec::Raw)
                    continue;
                if (chunk.storedBytes.empty() || chunk.entry.storedByteLength != chunk.storedBytes.size() ||
                    chunk.entry.decodedByteLength != chunk.storedBytes.size())
                    return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ArchiveEntryInvalid));
                if (chunk.storedBytes.size() > remainingWork)
                    return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
                remainingWork -= chunk.storedBytes.size();
                if (ComputeSha256(chunk.storedBytes) != chunk.entry.decodedHash)
                    return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ArchiveChunkHashMismatch));
            }
            return Result<std::uint64_t>::Success(remainingWork);
        }

        /** @brief Writes immutable metadata as raw existing-format entries. */
        void Metadata(std::vector<std::byte> &payload, const std::size_t index, const SaveArchiveEntryKind kind, const std::string &text,
                      const std::size_t dataOffset) {
            const auto bytes = std::as_bytes(std::span{text});
            SaveChunkDirectoryEntry entry;
            entry.storedByteLength = bytes.size();
            entry.decodedByteLength = bytes.size();
            entry.decodedHash = ComputeSha256(bytes);
            Entry(payload, index, kind, entry, payload.size() - dataOffset);
            payload.insert(payload.end(), bytes.begin(), bytes.end());
        }
    }  // namespace

    /** @copydoc SaveArchiveContainerWriter::Write */
    Result<FinalizedSaveArchive> SaveArchiveContainerWriter::Write(const SaveArchiveHeader &header, const SaveGameManifest &manifest,
                                                                   const std::span<const PreservedSaveChunk> chunks,
                                                                   const ArchiveFormatVersion version,
                                                                   const SaveArchiveReaderLimits &limits) {
        if ((version.Value() != 1 && version.Value() != 2) || limits.maximumEntries < 2 || chunks.size() > limits.maximumEntries - 2 ||
            chunks.size() >
                (std::numeric_limits<std::size_t>::max() - SaveArchiveContainerHeaderByteLength) / SaveArchiveContainerEntryByteLength - 2)
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        auto rawWork = ValidateRawChunks(chunks, limits.maximumReadWorkBytes);
        if (rawWork.HasError())
            return Result<FinalizedSaveArchive>::Failure(rawWork.ErrorValue());
        auto encodedHeader = EncodeSaveArchiveHeader(header, limits.metadata);
        if (encodedHeader.HasError())
            return Result<FinalizedSaveArchive>::Failure(encodedHeader.ErrorValue());
        auto encodedManifest = EncodeSaveGameManifest(manifest, limits.metadata);
        if (encodedManifest.HasError())
            return Result<FinalizedSaveArchive>::Failure(encodedManifest.ErrorValue());
        const auto count = chunks.size() + 2;
        const auto dataOffset = SaveArchiveContainerHeaderByteLength + count * SaveArchiveContainerEntryByteLength;
        std::uint64_t length = dataOffset;
        const auto admit = [&length, &limits](const std::uint64_t added) {
            if (length > limits.maximumStoredPayloadBytes || added > limits.maximumStoredPayloadBytes - length)
                return false;
            length += added;
            return true;
        };
        if (!admit(encodedHeader.Value().size()) || !admit(encodedManifest.Value().size()))
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        const auto firstChunkOffset = length;
        for (const auto &chunk : chunks)
            if (!chunk.entry.owner.IsValid() || chunk.entry.owner.Value().size() > 96 ||
                chunk.storedBytes.size() != chunk.entry.storedByteLength || !admit(chunk.storedBytes.size()))
                return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        auto padding = HeaderPadding(chunks, firstChunkOffset, limits.chunks.maximumAlignment);
        if (padding.HasError())
            return Result<FinalizedSaveArchive>::Failure(padding.ErrorValue());
        if (encodedHeader.Value().size() > limits.metadata.maximumHeaderBytes ||
            padding.Value() > limits.metadata.maximumHeaderBytes - encodedHeader.Value().size() || !admit(padding.Value()))
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        constexpr auto overhead = SaveArchivePreambleByteLength + SaveArchiveUnsignedTrailerByteLength;
        if (limits.maximumArchiveBytes < overhead || length > limits.maximumArchiveBytes - overhead)
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        try {
            auto ownedHeader = std::move(encodedHeader).Value();
            ownedHeader.append(padding.Value(), ' ');
            std::vector<std::byte> payload(dataOffset);
            payload.reserve(static_cast<std::size_t>(length));
            constexpr std::array containerMagic{std::byte{'H'}, std::byte{'S'}, std::byte{'C'}, std::byte{'T'},
                                                std::byte{'N'}, std::byte{'R'}, std::byte{'1'}, std::byte{0}};
            std::ranges::copy(containerMagic, payload.begin());
            Put(payload, 8, version.Value());
            Put(payload, 16, static_cast<std::uint64_t>(count));
            Put(payload, 24, static_cast<std::uint32_t>(SaveArchiveContainerEntryByteLength));
            Metadata(payload, 0, SaveArchiveEntryKind::Header, ownedHeader, dataOffset);
            Metadata(payload, 1, SaveArchiveEntryKind::Manifest, encodedManifest.Value(), dataOffset);
            for (std::size_t index = 0; index < chunks.size(); ++index) {
                Entry(payload, index + 2, SaveArchiveEntryKind::Chunk, chunks[index].entry, payload.size() - dataOffset);
                payload.insert(payload.end(), chunks[index].storedBytes.begin(), chunks[index].storedBytes.end());
            }
            std::vector<std::byte> preamble(SaveArchivePreambleByteLength);
            constexpr std::array archiveMagic{std::byte{'H'}, std::byte{'O'}, std::byte{'R'}, std::byte{'O'},
                                              std::byte{'S'}, std::byte{'A'}, std::byte{'V'}, std::byte{'E'}};
            std::ranges::copy(archiveMagic, preamble.begin());
            Put(preamble, 8, version.Value());
            Put(preamble, 16, length);
            Put(preamble, 24, static_cast<std::uint32_t>(SaveArchiveUnsignedTrailerByteLength));
            auto integrity = FinalizeSaveArchiveIntegrity(preamble, payload);
            if (integrity.HasError())
                return Result<FinalizedSaveArchive>::Failure(integrity.ErrorValue());
            auto archive = std::make_shared<std::vector<std::byte>>();
            archive->reserve(static_cast<std::size_t>(length) + overhead);
            archive->insert(archive->end(), preamble.begin(), preamble.end());
            archive->insert(archive->end(), payload.begin(), payload.end());
            archive->resize(archive->size() + SaveArchiveUnsignedTrailerByteLength);
            std::ranges::transform(integrity.Value().archiveContent.value.bytes, archive->begin() + SaveArchivePreambleByteLength + length,
                                   [](const std::uint8_t byte) {
                return static_cast<std::byte>(byte);
            });
            std::shared_ptr<const std::vector<std::byte>> owned = archive;
            auto readerLimits = limits;
            readerLimits.maximumReadWorkBytes = rawWork.Value();
            auto validated = SaveArchiveReader{readerLimits}.Read(owned);
            if (validated.HasError())
                return Result<FinalizedSaveArchive>::Failure(validated.ErrorValue());
            return Result<FinalizedSaveArchive>::Success(FinalizedSaveArchive{std::move(owned),
                                                                              {.integrity = integrity.Value(),
                                                                               .canonicalState = manifest.canonicalState,
                                                                               .archiveByteLength = archive->size(),
                                                                               .payloadByteLength = length,
                                                                               .entryCount = chunks.size()}});
        } catch (const std::bad_alloc &) {
            return Result<FinalizedSaveArchive>::Failure(MakeError(SaveErrors::CanonicalCodecAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
