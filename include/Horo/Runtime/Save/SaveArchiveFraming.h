#pragma once

/**
 * @file SaveArchiveFraming.h
 * @brief Bounded save chunk directory validation and selective payload access.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Save/SaveArchiveMetadata.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    inline constexpr std::size_t SaveArchivePreambleByteLength = 32;
    inline constexpr std::uint32_t SaveArchiveUnsignedTrailerByteLength = 52;
    inline constexpr std::uint32_t SaveArchiveSignedTrailerByteLength = 116;

    /** @brief Versioned hash algorithm identity used by archive and entry integrity claims. */
    enum class SaveIntegrityHashAlgorithm : std::uint16_t {
        Sha256 = 1,
    };

    /** @brief Algorithm and domain-separation version carried by an integrity claim. */
    struct SaveIntegrityAlgorithmVersion final {
        SaveIntegrityHashAlgorithm algorithm{SaveIntegrityHashAlgorithm::Sha256};
        std::uint16_t version{1};

        [[nodiscard]] constexpr auto operator<=>(const SaveIntegrityAlgorithmVersion &) const noexcept = default;
    };

    /** @brief Finalized coverage evidence for one immutable archive, excluding only its explicit trailer. */
    struct SaveArchiveIntegrityManifest final {
        SaveIntegrityAlgorithmVersion algorithm; /**< Algorithm for the archive and all entry digests. */
        ArchiveContentHash archiveContent;       /**< Digest of the exact preamble followed by exact stored payload bytes. */
        std::uint64_t preambleByteLength{};      /**< Covered preamble length; v1 requires 32 bytes. */
        std::uint64_t payloadByteLength{};       /**< Covered stored payload length. */
        std::uint32_t trailerByteLength{};       /**< Explicitly excluded trailer length, still required by framing. */

        [[nodiscard]] constexpr auto operator<=>(const SaveArchiveIntegrityManifest &) const noexcept = default;
    };

    /** @brief Storage codec applied to one independently addressable archive chunk. */
    enum class SaveChunkCodec : std::uint16_t {
        Raw = 0,
    };

    /** @brief Explicit admission limits for an untrusted archive chunk directory. */
    struct SaveChunkDirectoryLimits final {
        std::size_t maximumEntries{16'384};                   /**< Maximum directory records. */
        std::uint64_t maximumPayloadBytes{4ULL << 30U};       /**< Maximum aggregate stored payload bytes. */
        std::uint64_t maximumDecodedChunkBytes{64ULL << 20U}; /**< Maximum decoded bytes for one chunk. */
        std::uint32_t maximumAlignment{4'096};                /**< Maximum supported power-of-two alignment. */
    };

    /** @brief Validated-on-admission framing metadata for one manifest-owned chunk. */
    struct SaveChunkDirectoryEntry final {
        SaveRecordId record;                       /**< Globally unique stable chunk identity. */
        SaveParticipantId owner;                   /**< Manifest participant that owns the chunk. */
        std::uint64_t offset{};                    /**< Byte offset relative to the payload region. */
        std::uint64_t storedByteLength{};          /**< Exact bytes occupied in the archive. */
        std::uint64_t decodedByteLength{};         /**< Exact bytes after codec processing. */
        std::uint32_t alignment{1};                /**< Required power-of-two payload alignment. */
        SaveChunkCodec codec{SaveChunkCodec::Raw}; /**< Storage codec identity. */
        Sha256Digest decodedHash;                  /**< SHA-256 of canonical decoded data. */

        [[nodiscard]] auto operator<=>(const SaveChunkDirectoryEntry &) const noexcept = default;
    };

    /** @brief A bounded directory that can be inspected without loading chunk payloads. */
    struct SaveChunkDirectory final {
        SaveIntegrityAlgorithmVersion integrityAlgorithm; /**< Versioned algorithm for every entry digest. */
        std::uint64_t payloadByteLength{};                /**< Exact stored payload region size. */
        std::vector<SaveChunkDirectoryEntry> entries;     /**< Stable record-identity ordered entries. */

        [[nodiscard]] auto operator<=>(const SaveChunkDirectory &) const noexcept = default;
    };

    /** @brief Immutable owned proof that a chunk directory passed full manifest and framing validation. */
    class ValidatedSaveChunkDirectory final {
    public:
        /** @brief Returns the exact admitted payload size. @return Stored payload byte count. */
        [[nodiscard]] std::uint64_t PayloadByteLength() const noexcept;
        /** @brief Returns the versioned algorithm used by every admitted entry digest. @return Integrity algorithm identity. */
        [[nodiscard]] SaveIntegrityAlgorithmVersion IntegrityAlgorithm() const noexcept;
        /** @brief Returns stable record-ordered validated entries. @return Borrowed immutable directory entries. */
        [[nodiscard]] std::span<const SaveChunkDirectoryEntry> Entries() const noexcept;

    private:
        explicit ValidatedSaveChunkDirectory(SaveChunkDirectory directory);
        SaveChunkDirectory directory_;

        friend Result<ValidatedSaveChunkDirectory> ValidateSaveChunkDirectory(SaveChunkDirectory, const SaveGameManifest &,
                                                                              const SaveChunkDirectoryLimits &);
    };

    /**
     * @brief Validates directory bounds, layout, ownership, checksums, and manifest correspondence.
     * @param directory Untrusted directory metadata, consumed into an immutable validated token on success.
     * @param manifest Already validated canonical save manifest.
     * @param limits Trusted admission limits.
     * @return Owned validated directory token, or a stable framing error.
     */
    [[nodiscard]] Result<ValidatedSaveChunkDirectory> ValidateSaveChunkDirectory(SaveChunkDirectory directory,
                                                                                 const SaveGameManifest &manifest,
                                                                                 const SaveChunkDirectoryLimits &limits = {});

    /**
     * @brief Computes the domain-separated logical-state identity from canonical record bytes.
     *
     * This protects canonical logical records only. It contains no slot or publication identity and
     * must not be used as a slot generation or conflict token.
     * @param canonicalState Exact canonical state stream, already ordered by its schema.
     * @return Canonical logical-state digest.
     */
    [[nodiscard]] CanonicalStateHash ComputeCanonicalStateHash(std::span<const std::byte> canonicalState) noexcept;

    /**
     * @brief Computes the domain-separated identity of finalized archive bytes covered by v1.
     *
     * The covered bytes are exactly the preamble followed by the stored payload. The integrity
     * trailer is excluded only because its digest field would otherwise be self-referential; its
     * length remains part of the explicit framing evidence.
     * @param preamble Finalized archive preamble bytes.
     * @param payload Finalized stored payload bytes.
     * @return Archive-content digest, distinct from logical state and publication identity.
     */
    [[nodiscard]] ArchiveContentHash ComputeArchiveContentHash(std::span<const std::byte> preamble,
                                                               std::span<const std::byte> payload) noexcept;

    /**
     * @brief Builds immutable coverage evidence after archive finalization and before publication.
     * @param preamble Finalized archive preamble; v1 requires 32 bytes.
     * @param payload Finalized stored payload bytes.
     * @param trailerByteLength Exact encoded trailer length, excluded from the digest but not from framing.
     * @param algorithm Versioned hash algorithm selected before finalization.
     * @return Integrity manifest or a bounded coverage/algorithm error.
     */
    [[nodiscard]] Result<SaveArchiveIntegrityManifest> FinalizeSaveArchiveIntegrity(
        std::span<const std::byte> preamble, std::span<const std::byte> payload,
        std::uint32_t trailerByteLength = SaveArchiveUnsignedTrailerByteLength, SaveIntegrityAlgorithmVersion algorithm = {});

    /**
     * @brief Verifies the complete immutable archive before decoding or restore migration.
     *
     * This verifies exact file length and the whole covered byte range. Each manifest-owned entry
     * is independently verified by SelectSaveChunkPayload immediately before detached decode,
     * preserving narrow participant/record diagnostics without hashing the payload twice here.
     * @param integrity Finalized coverage evidence read from trusted framing/trailer state.
     * @param archive Complete archive bytes including the explicitly excluded trailer.
     * @param directory Previously validated manifest-corresponding entry directory.
     * @return Success or the narrowest available framing or whole-archive corruption error.
     */
    [[nodiscard]] Result<void> VerifySaveArchiveIntegrity(const SaveArchiveIntegrityManifest &integrity, std::span<const std::byte> archive,
                                                          const ValidatedSaveChunkDirectory &directory);

    /**
     * @brief Verifies the logical canonical-state digest before migration or world mutation.
     * @param expected Expected typed logical-state digest from the validated manifest.
     * @param canonicalState Exact decoded canonical record stream.
     * @return Success or SaveErrors::CanonicalStateHashMismatch.
     */
    [[nodiscard]] Result<void> VerifyCanonicalStateHash(const CanonicalStateHash &expected, std::span<const std::byte> canonicalState);

    /**
     * @brief Returns one verified raw chunk without copying or decoding unrelated payloads.
     * @param payload Exact payload region bytes.
     * @param directory Immutable directory token validated once before any selections.
     * @param record Desired stable chunk identity.
     * @return Borrowed verified bytes, or an empty optional when the record is unknown and may be skipped.
     */
    [[nodiscard]] Result<std::optional<std::span<const std::byte>>> SelectSaveChunkPayload(std::span<const std::byte> payload,
                                                                                           const ValidatedSaveChunkDirectory &directory,
                                                                                           SaveRecordId record);
}  // namespace Horo::Runtime
