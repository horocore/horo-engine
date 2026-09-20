#include "Horo/Runtime/Save/SaveArchiveFraming.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Runtime {
    namespace {
        constexpr std::string_view CanonicalStateHashTag = "HoroSave.CanonicalState.v1";
        constexpr std::string_view ArchiveContentHashTag = "HoroSave.ArchiveContent.v1";

        /** @brief Reports whether the declared integrity algorithm and domain version are supported. */
        [[nodiscard]] constexpr bool IsSupported(const SaveIntegrityAlgorithmVersion algorithm) noexcept {
            return algorithm.algorithm == SaveIntegrityHashAlgorithm::Sha256 && algorithm.version == 1;
        }

        /** @brief Reports whether the trailer length is one of the explicitly framed v1 forms. */
        [[nodiscard]] constexpr bool IsSupportedTrailerLength(const std::uint32_t length) noexcept {
            return length == SaveArchiveUnsignedTrailerByteLength || length == SaveArchiveSignedTrailerByteLength;
        }

        /** @brief Computes one domain-separated digest without copying the covered archive bytes. */
        [[nodiscard]] Sha256Digest ComputeDomainSeparatedSha256(const std::string_view tag, const std::span<const std::byte> first,
                                                                const std::span<const std::byte> second = {}) noexcept {
            const auto tagBytes = std::as_bytes(std::span<const char>{tag.data(), tag.size()});
            const std::array<std::byte, 1> terminator{};
            const std::array fragments{tagBytes, std::span<const std::byte>{terminator}, first, second};
            return ComputeSha256Fragments(std::span<const std::span<const std::byte>>{fragments});
        }

        /** @brief Adds stable participant/record context without retaining corrupted payload bytes. */
        void AddEntryDiagnostic(Error &error, const SaveChunkDirectoryEntry &entry) {
            const auto column =
                static_cast<std::uint32_t>(std::min(entry.offset, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
            error.diagnostics.push_back({DiagnosticCode{"save.archive.entry_integrity"},
                                         DiagnosticSeverity::Error,
                                         "Covered save entry failed integrity verification.",
                                         {"archive", 0, column},
                                         "participant/" + entry.owner.Value() + "/record/" + entry.record.ToString()});
        }

        /** @brief Checks algorithm, explicit coverage sizes, and the directory's digest contract. */
        [[nodiscard]] Result<void> ValidateIntegrityContract(const SaveArchiveIntegrityManifest &integrity,
                                                             const ValidatedSaveChunkDirectory &directory) {
            if (!IsSupported(integrity.algorithm) || !IsSupported(directory.IntegrityAlgorithm()))
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveIntegrityAlgorithmUnsupported));
            if (integrity.algorithm != directory.IntegrityAlgorithm() || integrity.preambleByteLength != SaveArchivePreambleByteLength ||
                integrity.payloadByteLength != directory.PayloadByteLength() || !IsSupportedTrailerLength(integrity.trailerByteLength))
                return Result<void>::Failure(MakeError(SaveErrors::ArchiveIntegrityCoverageInvalid));
            return Result<void>::Success();
        }

        /** @brief Checks that the complete byte span contains exactly the declared covered bytes and trailer. */
        [[nodiscard]] Result<void> ValidateArchiveLength(const SaveArchiveIntegrityManifest &integrity,
                                                         const std::span<const std::byte> archive) {
            const std::uint64_t preambleLength = integrity.preambleByteLength;
            const std::uint64_t payloadLength = integrity.payloadByteLength;
            if (const std::uint64_t trailerLength = integrity.trailerByteLength;
                preambleLength > std::numeric_limits<std::uint64_t>::max() - payloadLength ||
                preambleLength + payloadLength > std::numeric_limits<std::uint64_t>::max() - trailerLength ||
                preambleLength + payloadLength + trailerLength != archive.size())
                return Result<void>::Failure(MakeError(SaveErrors::ArchivePayloadTruncated));
            return Result<void>::Success();
        }

        /** @brief Reports whether directory limits are finite and internally coherent. */
        [[nodiscard]] bool HasValidLimits(const SaveChunkDirectoryLimits &limits) noexcept {
            return limits.maximumEntries != 0 && limits.maximumPayloadBytes != 0 && limits.maximumDecodedChunkBytes != 0 &&
                   limits.maximumAlignment != 0 && std::has_single_bit(limits.maximumAlignment);
        }

        /** @brief Looks up the manifest owner of one chunk identity. */
        [[nodiscard]] const SaveParticipantId *FindManifestOwner(const SaveGameManifest &manifest, const SaveRecordId record) noexcept {
            for (const SaveManifestParticipant &participant : manifest.participants) {
                if (std::ranges::binary_search(participant.chunks, record))
                    return &participant.participant;
            }
            return nullptr;
        }

        /** @brief Validates one entry's identity, codec, and alignment fields. */
        [[nodiscard]] bool HasValidEntryIdentity(const SaveChunkDirectoryEntry &entry, const SaveChunkDirectoryLimits &limits) noexcept {
            return entry.record.IsValid() && entry.owner.IsValid() && entry.alignment != 0 && entry.alignment <= limits.maximumAlignment &&
                   std::has_single_bit(entry.alignment) && entry.codec == SaveChunkCodec::Raw;
        }

        /** @brief Validates one entry's length fields and checked end offset. */
        [[nodiscard]] bool HasValidEntryLengths(const SaveChunkDirectoryEntry &entry, const SaveChunkDirectoryLimits &limits) noexcept {
            return entry.storedByteLength != 0 && entry.decodedByteLength != 0 &&
                   entry.decodedByteLength <= limits.maximumDecodedChunkBytes && entry.storedByteLength == entry.decodedByteLength &&
                   entry.offset <= std::numeric_limits<std::uint64_t>::max() - entry.storedByteLength;
        }

        /** @brief Counts manifest chunks without touching archive payload bytes. */
        [[nodiscard]] std::size_t CountManifestChunks(const SaveGameManifest &manifest) noexcept {
            std::size_t count = 0;
            for (const SaveManifestParticipant &participant : manifest.participants)
                count += participant.chunks.size();
            return count;
        }

        /** @brief Validates one entry against its expected contiguous payload position. */
        [[nodiscard]] bool HasValidEntryLayout(const SaveChunkDirectoryEntry &entry, const std::uint64_t expectedOffset,
                                               const std::uint64_t payloadByteLength, const SaveChunkDirectoryLimits &limits) noexcept {
            return HasValidEntryIdentity(entry, limits) && HasValidEntryLengths(entry, limits) && entry.offset == expectedOffset &&
                   entry.offset % entry.alignment == 0 && entry.offset + entry.storedByteLength <= payloadByteLength;
        }

        /** @brief Validates the complete contiguous entry sequence and ownership mapping. */
        [[nodiscard]] bool HasValidEntries(const SaveChunkDirectory &directory, const SaveGameManifest &manifest,
                                           const SaveChunkDirectoryLimits &limits) noexcept {
            const std::uint64_t dataLength = directory.dataByteLength == 0 ? directory.payloadByteLength : directory.dataByteLength;
            if (directory.dataByteOffset > directory.payloadByteLength ||
                dataLength > directory.payloadByteLength - directory.dataByteOffset)
                return false;
            std::uint64_t expectedOffset = directory.dataByteOffset;
            const SaveRecordId *previousRecord = nullptr;
            for (const SaveChunkDirectoryEntry &entry : directory.entries) {
                const bool layoutValid = HasValidEntryLayout(entry, expectedOffset, directory.payloadByteLength, limits);
                const bool orderValid = previousRecord == nullptr || *previousRecord < entry.record;
                if (const SaveParticipantId *owner = FindManifestOwner(manifest, entry.record);
                    !layoutValid || !orderValid || owner == nullptr || *owner != entry.owner)
                    return false;
                expectedOffset += entry.storedByteLength;
                previousRecord = &entry.record;
            }
            return expectedOffset == directory.dataByteOffset + dataLength;
        }
    }  // namespace

    ValidatedSaveChunkDirectory::ValidatedSaveChunkDirectory(SaveChunkDirectory directory) : directory_(std::move(directory)) {}

    /** @copydoc ValidatedSaveChunkDirectory::PayloadByteLength */
    std::uint64_t ValidatedSaveChunkDirectory::PayloadByteLength() const noexcept {
        return directory_.payloadByteLength;
    }

    /** @copydoc ValidatedSaveChunkDirectory::IntegrityAlgorithm */
    SaveIntegrityAlgorithmVersion ValidatedSaveChunkDirectory::IntegrityAlgorithm() const noexcept {
        return directory_.integrityAlgorithm;
    }

    /** @copydoc ValidatedSaveChunkDirectory::Entries */
    std::span<const SaveChunkDirectoryEntry> ValidatedSaveChunkDirectory::Entries() const noexcept {
        return directory_.entries;
    }

    /** @copydoc ValidateSaveChunkDirectory */
    Result<ValidatedSaveChunkDirectory> ValidateSaveChunkDirectory(SaveChunkDirectory directory, const SaveGameManifest &manifest,
                                                                   const SaveChunkDirectoryLimits &limits) {
        if (!IsSupported(directory.integrityAlgorithm))
            return Result<ValidatedSaveChunkDirectory>::Failure(MakeError(SaveErrors::ArchiveIntegrityAlgorithmUnsupported));
        if (!HasValidLimits(limits) || directory.payloadByteLength > limits.maximumPayloadBytes ||
            directory.payloadByteLength > std::numeric_limits<std::size_t>::max() || directory.entries.size() > limits.maximumEntries)
            return Result<ValidatedSaveChunkDirectory>::Failure(MakeError(SaveErrors::ArchiveFramingLimitExceeded));
        if (directory.entries.empty())
            return Result<ValidatedSaveChunkDirectory>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));

        if (directory.entries.size() != CountManifestChunks(manifest))
            return Result<ValidatedSaveChunkDirectory>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
        if (!HasValidEntries(directory, manifest, limits))
            return Result<ValidatedSaveChunkDirectory>::Failure(MakeError(SaveErrors::ArchiveDirectoryInvalid));
        return Result<ValidatedSaveChunkDirectory>::Success(ValidatedSaveChunkDirectory{std::move(directory)});
    }

    /** @copydoc ComputeCanonicalStateHash */
    CanonicalStateHash ComputeCanonicalStateHash(const std::span<const std::byte> canonicalState) noexcept {
        return {.value = ComputeDomainSeparatedSha256(CanonicalStateHashTag, canonicalState)};
    }

    /** @copydoc ComputeArchiveContentHash */
    ArchiveContentHash ComputeArchiveContentHash(const std::span<const std::byte> preamble,
                                                 const std::span<const std::byte> payload) noexcept {
        return {.value = ComputeDomainSeparatedSha256(ArchiveContentHashTag, preamble, payload)};
    }

    /** @copydoc FinalizeSaveArchiveIntegrity */
    Result<SaveArchiveIntegrityManifest> FinalizeSaveArchiveIntegrity(const std::span<const std::byte> preamble,
                                                                      const std::span<const std::byte> payload,
                                                                      const std::uint32_t trailerByteLength,
                                                                      const SaveIntegrityAlgorithmVersion algorithm) {
        if (!IsSupported(algorithm))
            return Result<SaveArchiveIntegrityManifest>::Failure(MakeError(SaveErrors::ArchiveIntegrityAlgorithmUnsupported));
        if (preamble.size() != SaveArchivePreambleByteLength || !IsSupportedTrailerLength(trailerByteLength))
            return Result<SaveArchiveIntegrityManifest>::Failure(MakeError(SaveErrors::ArchiveIntegrityCoverageInvalid));
        return Result<SaveArchiveIntegrityManifest>::Success({.algorithm = algorithm,
                                                              .archiveContent = ComputeArchiveContentHash(preamble, payload),
                                                              .preambleByteLength = preamble.size(),
                                                              .payloadByteLength = payload.size(),
                                                              .trailerByteLength = trailerByteLength});
    }

    /** @copydoc VerifySaveArchiveIntegrity */
    Result<void> VerifySaveArchiveIntegrity(const SaveArchiveIntegrityManifest &integrity, const std::span<const std::byte> archive,
                                            const ValidatedSaveChunkDirectory &directory) {
        if (const auto valid = VerifySaveArchiveIntegrity(integrity, archive); valid.HasError())
            return valid;
        if (const auto valid = ValidateIntegrityContract(integrity, directory); valid.HasError())
            return valid;
        return Result<void>::Success();
    }

    /** @copydoc VerifySaveArchiveIntegrity */
    Result<void> VerifySaveArchiveIntegrity(const SaveArchiveIntegrityManifest &integrity, const std::span<const std::byte> archive) {
        if (!IsSupported(integrity.algorithm) || integrity.preambleByteLength != SaveArchivePreambleByteLength ||
            !IsSupportedTrailerLength(integrity.trailerByteLength))
            return Result<void>::Failure(MakeError(SaveErrors::ArchiveIntegrityCoverageInvalid));
        if (const auto valid = ValidateArchiveLength(integrity, archive); valid.HasError())
            return valid;

        const auto preamble = archive.first(static_cast<std::size_t>(integrity.preambleByteLength));
        if (const auto payload = archive.subspan(static_cast<std::size_t>(integrity.preambleByteLength),
                                                 static_cast<std::size_t>(integrity.payloadByteLength));
            ComputeArchiveContentHash(preamble, payload) != integrity.archiveContent) {
            Error error = MakeError(SaveErrors::ArchiveContentHashMismatch);
            error.diagnostics.push_back({DiagnosticCode{"save.archive.content_integrity"},
                                         DiagnosticSeverity::Error,
                                         "Finalized preamble and stored payload bytes failed whole-archive verification.",
                                         {"archive", 0, 0},
                                         "preamble+payload"});
            return Result<void>::Failure(std::move(error));
        }

        return Result<void>::Success();
    }

    /** @copydoc VerifyCanonicalStateHash */
    Result<void> VerifyCanonicalStateHash(const CanonicalStateHash &expected, const std::span<const std::byte> canonicalState) {
        if (ComputeCanonicalStateHash(canonicalState) == expected)
            return Result<void>::Success();
        Error error = MakeError(SaveErrors::CanonicalStateHashMismatch);
        error.diagnostics.push_back({DiagnosticCode{"save.canonical_state.integrity"},
                                     DiagnosticSeverity::Error,
                                     "Canonical logical record bytes failed verification before migration.",
                                     {"canonical", 0, 0},
                                     "canonical-state"});
        return Result<void>::Failure(std::move(error));
    }

    /** @copydoc SelectSaveChunkPayload */
    Result<std::optional<std::span<const std::byte>>> SelectSaveChunkPayload(const std::span<const std::byte> payload,
                                                                             const ValidatedSaveChunkDirectory &directory,
                                                                             const SaveRecordId record) {
        if (payload.size() != directory.PayloadByteLength())
            return Result<std::optional<std::span<const std::byte>>>::Failure(MakeError(SaveErrors::ArchivePayloadTruncated));
        const std::span entries = directory.Entries();
        const auto found = std::ranges::lower_bound(entries, record, {}, &SaveChunkDirectoryEntry::record);
        if (found == entries.end() || found->record != record)
            return Result<std::optional<std::span<const std::byte>>>::Success(std::nullopt);
        const auto bytes = payload.subspan(static_cast<std::size_t>(found->offset), static_cast<std::size_t>(found->storedByteLength));
        if (ComputeSha256(bytes) != found->decodedHash) {
            Error error = MakeError(SaveErrors::ArchiveChunkHashMismatch);
            AddEntryDiagnostic(error, *found);
            return Result<std::optional<std::span<const std::byte>>>::Failure(std::move(error));
        }
        return Result<std::optional<std::span<const std::byte>>>::Success(bytes);
    }
}  // namespace Horo::Runtime
