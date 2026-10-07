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
#include <type_traits>
#include <utility>
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

    /** @brief Reports a bounded archive read-work failure with its stable diagnostic code. */
    [[nodiscard]] inline Error ReadWorkError(const std::size_t offset, const std::string_view path) {
        Error error = ReaderError(SaveErrors::ArchiveFramingLimitExceeded, offset, path);
        error.diagnostics.front().code = DiagnosticCode{"save.archive.limit.read_work"};
        error.diagnostics.front().message = "Archive read work budget exceeded.";
        return error;
    }

    /** @brief Reads one little-endian unsigned field without advancing past the input. */
    template <typename Value>
    [[nodiscard]] bool ReadLittleEndian(const std::span<const std::byte> bytes, std::size_t &offset, Value &value) noexcept {
        static_assert(std::is_unsigned_v<Value>);
        if (offset > bytes.size() || sizeof(Value) > bytes.size() - offset)
            return false;
        value = 0;
        for (std::size_t index = 0; index < sizeof(Value); ++index)
            value |= static_cast<Value>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
        offset += sizeof(Value);
        return true;
    }

    /** @brief Reads a fixed-size byte field without advancing past the input. */
    [[nodiscard]] inline bool ReadArray(const std::span<const std::byte> bytes, std::size_t &offset,
                                        const std::span<std::uint8_t> destination) noexcept {
        if (offset > bytes.size() || destination.size() > bytes.size() - offset)
            return false;
        for (std::size_t index = 0; index < destination.size(); ++index)
            destination[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
        offset += destination.size();
        return true;
    }

    /** @brief Checks whether a fixed-size identity or key is entirely zero. */
    [[nodiscard]] inline bool IsZero(
        const std::span<const std::uint8_t> bytes) noexcept {  // NOSONAR(cpp:S1144) -- identity fields use uint8_t storage.
        return std::ranges::all_of(bytes, [](const std::uint8_t value) {
            return value == 0;
        });
    }

    /** @brief Parses and validates the versioned envelope and trailer before container admission. */
    [[nodiscard]] Result<std::pair<SaveArchivePreamble, SaveArchiveIntegrityManifest>> ReadEnvelope(std::span<const std::byte> archive,
                                                                                                    const SaveArchiveReaderLimits &limits,
                                                                                                    SaveArchiveSignatureInfo &signature);

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

    /** @brief Closed target-private codec admission; ordinary validation stays installed-only. */
    enum class DirectoryCodecAdmission : std::uint8_t {
        SupportedOnly,
        OptionalOpaque
    };

    /** @brief Reader-owned directory construction after actual container/manifest decode. Not an installed API. */
    class DirectoryAdmission final {
    public:
        /**
         * @brief Validates storage using codec authority derived from the actual archive metadata.
         * @param directory Untrusted storage directory.
         * @param manifest Parsed canonical manifest that owns every chunk.
         * @param limits Finite directory bounds.
         * @param admission Closed reader-only capability; ordinary callers use SupportedOnly.
         * @param archiveVersion Actual admitted container version.
         * @return Immutable directory proof or a typed framing/codec failure.
         */
        [[nodiscard]] static Result<ValidatedSaveChunkDirectory> Validate(SaveChunkDirectory directory, const SaveGameManifest &manifest,
                                                                          const SaveChunkDirectoryLimits &limits,
                                                                          DirectoryCodecAdmission admission, std::uint32_t archiveVersion);
    };

    [[nodiscard]] Result<SaveParticipantId> DecodeOwner(const RawEntry &entry);
    [[nodiscard]] Result<DecodedMetadata> DecodeMetadata(std::span<const std::byte> payload, const std::vector<RawEntry> &entries,
                                                         const SaveArchiveMetadataLimits &limits);
    [[nodiscard]] Result<ValidatedSaveChunkDirectory> BuildDirectory(std::span<const std::byte> payload,
                                                                     const std::vector<RawEntry> &entries, const SaveGameManifest &manifest,
                                                                     const SaveArchiveReaderLimits &limits, std::uint32_t archiveVersion);
}  // namespace Horo::Runtime::SaveArchiveReaderDetail
