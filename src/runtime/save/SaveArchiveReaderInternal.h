#pragma once

#include "Horo/Runtime/Save/SaveArchiveReader.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Runtime::SaveArchiveReaderDetail {
    struct RawEntry final {
        SaveArchiveEntryKind kind{};
        std::uint16_t codec{};
        std::array<std::uint8_t, 16> record{};
        std::uint16_t ownerLength{};
        std::array<std::uint8_t, MaximumSaveParticipantIdBytes> owner{};
        std::uint64_t relativeOffset{};
        std::uint64_t storedByteLength{};
        std::uint64_t decodedByteLength{};
        std::uint32_t alignment{};
        Sha256Digest decodedHash;
        std::size_t absoluteOffset{};
    };

    struct DecodedMetadata final {
        SaveArchiveHeader header;
        SaveGameManifest manifest;
    };

    [[nodiscard]] Result<SaveParticipantId> DecodeOwner(const RawEntry &entry);
    [[nodiscard]] Result<DecodedMetadata> DecodeMetadata(std::span<const std::byte> payload, const std::vector<RawEntry> &entries,
                                                         const SaveArchiveMetadataLimits &limits);
    [[nodiscard]] Result<ValidatedSaveChunkDirectory> BuildDirectory(std::span<const std::byte> payload,
                                                                     const std::vector<RawEntry> &entries, const SaveGameManifest &manifest,
                                                                     const SaveArchiveReaderLimits &limits);
    [[nodiscard]] Result<void> VerifyDecodedChunks(std::span<const std::byte> payload, const ValidatedSaveChunkDirectory &directory,
                                                   const SaveArchiveReaderLimits &limits);
}  // namespace Horo::Runtime::SaveArchiveReaderDetail
