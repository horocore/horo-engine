#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveArchiveReaderInternal.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime::SaveArchiveReaderDetail {
    namespace {
        [[nodiscard]] Error ReaderError(const ErrorCodeDescriptor &descriptor, const std::size_t offset,
                                        const std::string_view path = "archive") {
            Error error = MakeError(descriptor);
            error.diagnostics.push_back(
                {DiagnosticCode{"save.archive.reader.location"},
                 DiagnosticSeverity::Error,
                 std::string{descriptor.summary},
                 {"archive", 0,
                  static_cast<std::uint32_t>(std::min(offset, static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())))},
                 std::string{path}});
            return error;
        }

        [[nodiscard]] bool IsSafeLinkText(const std::string_view text) noexcept {
            return IsValidUtf8ScalarSequence(text) && text.find('/') == std::string_view::npos &&
                   text.find('\\') == std::string_view::npos && text.find("..") == std::string_view::npos &&
                   text.find("://") == std::string_view::npos;
        }
    }  // namespace

    Result<SaveParticipantId> DecodeOwner(const RawEntry &entry) {
        const std::string_view text{reinterpret_cast<const char *>(entry.owner.data()), entry.ownerLength};
        if (!IsSafeLinkText(text))
            return Result<SaveParticipantId>::Failure(ReaderError(SaveErrors::ArchiveUnsafeReference, entry.absoluteOffset, "entry/owner"));
        auto parsed = SaveParticipantId::Parse(text);
        if (parsed.HasError())
            return Result<SaveParticipantId>::Failure(ReaderError(SaveErrors::ArchiveEntryInvalid, entry.absoluteOffset, "entry/owner"));
        return parsed;
    }

    Result<DecodedMetadata> DecodeMetadata(const std::span<const std::byte> payload, const std::vector<RawEntry> &entries,
                                           const SaveArchiveMetadataLimits &limits) {
        for (std::size_t index = 2; index < entries.size(); ++index) {
            if (entries[index].kind != SaveArchiveEntryKind::Chunk)
                return Result<DecodedMetadata>::Failure(
                    ReaderError(SaveErrors::ArchiveEntryInvalid, index * SaveArchiveContainerEntryByteLength, "container/entryOrder"));
        }
        const RawEntry &headerEntry = entries[0];
        const RawEntry &manifestEntry = entries[1];
        const auto headerBytes = payload.subspan(headerEntry.absoluteOffset, static_cast<std::size_t>(headerEntry.storedByteLength));
        const auto manifestBytes = payload.subspan(manifestEntry.absoluteOffset, static_cast<std::size_t>(manifestEntry.storedByteLength));
        auto header = DecodeSaveArchiveHeader(std::string_view{reinterpret_cast<const char *>(headerBytes.data()), headerBytes.size()},
                                              limits);  // NOSONAR(cpp:S6022) -- validated byte payload is passed to the text decoder.
        if (header.HasError())
            return Result<DecodedMetadata>::Failure(std::move(header).ErrorValue());
        auto manifest = DecodeSaveGameManifest(std::string_view{reinterpret_cast<const char *>(manifestBytes.data()), manifestBytes.size()},
                                               limits);  // NOSONAR(cpp:S6022) -- validated byte payload is passed to the text decoder.
        if (manifest.HasError())
            return Result<DecodedMetadata>::Failure(std::move(manifest).ErrorValue());
        return Result<DecodedMetadata>::Success({std::move(header).Value(), std::move(manifest).Value()});
    }

    Result<ValidatedSaveChunkDirectory> BuildDirectory(const std::span<const std::byte> payload, const std::vector<RawEntry> &entries,
                                                       const SaveGameManifest &manifest, const SaveArchiveReaderLimits &limits) {
        SaveChunkDirectory directory{.integrityAlgorithm = {},
                                     .payloadByteLength = payload.size(),
                                     .entries = {},
                                     .dataByteOffset = entries[2].absoluteOffset,
                                     .dataByteLength = payload.size() - entries[2].absoluteOffset};
        directory.entries.reserve(entries.size() - 2);
        for (std::size_t index = 2; index < entries.size(); ++index) {
            const RawEntry &raw = entries[index];
            auto owner = DecodeOwner(raw);
            if (owner.HasError())
                return Result<ValidatedSaveChunkDirectory>::Failure(std::move(owner).ErrorValue());
            auto record = SaveRecordId::FromBytes(raw.record);
            if (record.HasError())
                return Result<ValidatedSaveChunkDirectory>::Failure(
                    ReaderError(SaveErrors::ArchiveEntryInvalid, raw.absoluteOffset, "entry/record"));
            directory.entries.push_back({.record = std::move(record).Value(),
                                         .owner = std::move(owner).Value(),
                                         .offset = raw.absoluteOffset,
                                         .storedByteLength = raw.storedByteLength,
                                         .decodedByteLength = raw.decodedByteLength,
                                         .alignment = raw.alignment,
                                         .codec = SaveChunkCodec::Raw,
                                         .decodedHash = raw.decodedHash});
        }
        return ValidateSaveChunkDirectory(std::move(directory), manifest, limits.chunks);
    }

    Result<void> VerifyDecodedChunks(const std::span<const std::byte> payload, const ValidatedSaveChunkDirectory &directory,
                                     const SaveArchiveReaderLimits &limits) {
        std::uint64_t selectedBytes = 0;
        for (const auto &entry : directory.Entries()) {
            if (selectedBytes > limits.maximumDecodedBytes - entry.decodedByteLength)
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveDecompressionLimitExceeded));
            selectedBytes += entry.decodedByteLength;
            auto selected = SelectSaveChunkPayload(payload, directory, entry.record);
            if (selected.HasError())
                return Result<void>::Failure(std::move(selected).ErrorValue());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::SaveArchiveReaderDetail
