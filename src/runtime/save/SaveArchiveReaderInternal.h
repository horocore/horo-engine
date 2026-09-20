#pragma once

#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime::SaveArchiveReaderDetail {
    /** @brief Adds the archive byte offset and logical path to a reader failure. */
    [[nodiscard]] inline Error ReaderError(const ErrorCodeDescriptor &descriptor, const std::size_t offset,
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
}  // namespace Horo::Runtime::SaveArchiveReaderDetail
