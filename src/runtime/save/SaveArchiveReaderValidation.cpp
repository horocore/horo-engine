#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveArchiveReaderInternal.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime::SaveArchiveReaderDetail {
    namespace {
        [[nodiscard]] bool IsSafeLinkText(const std::string_view text) noexcept {
            return IsValidUtf8ScalarSequence(text) &&
                   !std::ranges::any_of(text,
                                        [](const char value) {
                return static_cast<unsigned char>(value) < 0x20U || value == '\x7f';
            }) && text.find('/') == std::string_view::npos &&
                   text.find('\\') == std::string_view::npos && text.find("..") == std::string_view::npos &&
                   text.find("://") == std::string_view::npos;
        }

        /** @brief Copies bounded byte-oriented metadata into the text decoder's character view. */
        [[nodiscard]] std::string CopyByteText(const std::span<const std::byte> bytes) {
            std::string text(bytes.size(), '\0');
            std::ranges::transform(bytes, text.begin(), [](const std::byte value) {
                return static_cast<char>(std::to_integer<unsigned char>(value));
            });
            return text;
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
        if (headerBytes.size() > limits.maximumHeaderBytes || manifestBytes.size() > limits.maximumManifestBytes)
            return Result<DecodedMetadata>::Failure(MakeError(SaveErrors::ArchiveMetadataLimitExceeded));
        const auto headerText = CopyByteText(headerBytes);
        auto header = DecodeSaveArchiveHeader(headerText, limits);
        if (header.HasError())
            return Result<DecodedMetadata>::Failure(header.ErrorValue());
        const auto manifestText = CopyByteText(manifestBytes);
        auto manifest = DecodeSaveGameManifest(manifestText, limits);
        if (manifest.HasError())
            return Result<DecodedMetadata>::Failure(manifest.ErrorValue());
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
                return Result<ValidatedSaveChunkDirectory>::Failure(owner.ErrorValue());
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

}  // namespace Horo::Runtime::SaveArchiveReaderDetail
