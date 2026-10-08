#pragma once

/**
 * @file SaveArchiveReader.h
 * @brief Backend-neutral bounded admission for finalized runtime-save archives.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveArchiveFraming.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    inline constexpr std::size_t SaveArchiveContainerHeaderByteLength = 32;
    inline constexpr std::size_t SaveArchiveContainerEntryByteLength = 188;

    /** @brief Canonical kind of one bounded payload entry. */
    enum class SaveArchiveEntryKind : std::uint8_t {
        Header = 1,
        Manifest = 2,
        Chunk = 3,
        Extension = 4,
    };

    /** @brief Signature algorithm descriptor carried by the finalized trailer. */
    enum class SaveArchiveSignatureAlgorithm : std::uint16_t {
        None = 0,
        Ed25519 = 1,
    };

    /** @brief Parsed v1 envelope fields; all byte lengths are checked before exposure. */
    struct SaveArchivePreamble final {
        ArchiveFormatVersion archiveFormatVersion;
        std::uint32_t flags{};
        std::uint64_t payloadByteLength{};
        std::uint32_t trailerByteLength{};
    };

    /** @brief Non-secret signature metadata; the reader does not select trust roots or verify keys. */
    struct SaveArchiveSignatureInfo final {
        SaveArchiveSignatureAlgorithm algorithm{SaveArchiveSignatureAlgorithm::None};
        std::array<std::uint8_t, 16> signerKeyId{};
        std::uint16_t signatureByteLength{};
    };

    /** @brief Exact stored unknown chunk and integrity evidence retained for a later archive. */
    struct PreservedSaveChunk final {
        SaveChunkDirectoryEntry entry;      /**< Record, owner, codec, lengths, alignment, and decoded digest; offset is layout-specific. */
        std::vector<std::byte> storedBytes; /**< Exact bytes from the integrity-verified source archive. */

        [[nodiscard]] auto operator<=>(const PreservedSaveChunk &) const noexcept = default;
    };

    /** @brief Explicit unknown-data disposition after compatibility preflight. */
    struct SaveUnknownDataReport final {
        std::vector<PreservedSaveChunk> preserved; /**< Stable record-ordered optional chunks that must survive repacking. */
        std::vector<SaveParticipantId> dropped;    /**< Optional owners explicitly authorized for omission by trusted policy. */
    };

    /** @brief Finite budgets applied to every untrusted archive admission path. */
    struct SaveArchiveReaderLimits final {
        std::size_t maximumArchiveBytes{
            static_cast<std::size_t>(std::min<std::uint64_t>(4ULL << 30U, std::numeric_limits<std::size_t>::max()))};
        std::uint64_t maximumStoredPayloadBytes{std::min<std::uint64_t>(4ULL << 30U, std::numeric_limits<std::size_t>::max())};
        std::uint64_t maximumDecodedBytes{64ULL << 20U};
        std::size_t maximumEntries{16'384};
        std::size_t maximumNestingDepth{8};
        std::uint64_t maximumExpansionRatio{64};
        std::uint64_t maximumReadWorkBytes{16ULL << 30U}; /**< Aggregate archive validation work admission. */
        SaveArchiveMetadataLimits metadata{};
        SaveChunkDirectoryLimits chunks{};
    };

    namespace SaveArchiveReaderDetail {
        struct Reader;
    }

    /**
     * @brief Complete archive admission proof with borrowed or explicitly owned source bytes.
     *
     * The span overload retains a borrow of the caller's immutable bytes. Use the shared-vector
     * overload when the storage backend owns the archive; selected canonical chunks own their
     * decoded storage independently of this archive's lifetime.
     */
    class ValidatedSaveArchive final {
    public:
        /** @brief Returns parsed envelope fields. @return Immutable preamble. */
        [[nodiscard]] const SaveArchivePreamble &Preamble() const noexcept;
        /** @brief Returns verified archive integrity evidence. @return Immutable coverage manifest. */
        [[nodiscard]] const SaveArchiveIntegrityManifest &Integrity() const noexcept;
        /** @brief Returns non-secret trailer signature metadata. @return Immutable signature descriptor. */
        [[nodiscard]] const SaveArchiveSignatureInfo &Signature() const noexcept;
        /** @brief Returns validated header metadata. @return Immutable header. */
        [[nodiscard]] const SaveArchiveHeader &Header() const noexcept;
        /** @brief Returns validated manifest metadata. @return Immutable manifest. */
        [[nodiscard]] const SaveGameManifest &Manifest() const noexcept;
        /** @brief Returns the validated manifest-owned chunk directory. @return Immutable directory proof. */
        [[nodiscard]] const ValidatedSaveChunkDirectory &Directory() const noexcept;
        /** @brief Returns the exact borrowed payload bytes. @return Complete payload region. */
        [[nodiscard]] std::span<const std::byte> Payload() const noexcept;
        /**
         * @brief Selects one already-integrity-verified chunk without invoking module code.
         * @param record Stable record identity.
         * @return Owned canonical chunk bytes or an empty optional for an unknown lookup. Unknown codecs always
         * return ArchiveCodecUnsupported, including optional opaque storage admitted by container v2/schema 2.
         * Structural archive admission grants no decoder or participant activation capability.
         */
        [[nodiscard]] Result<std::optional<std::vector<std::byte>>> SelectChunk(SaveRecordId record) const;
        /**
         * @brief Preflights required features and participants, then captures exact unknown optional chunks.
         * @param policy Sealed release policy; only its explicit droppable IDs permit omission.
         * @param maximumPreservedBytes Finite aggregate copy budget for opaque stored bytes.
         * @return Preserved bytes and explicit drops, or an actionable compatibility/integrity/limit error.
         */
        [[nodiscard]] Result<SaveUnknownDataReport> InspectUnknownData(const SaveCompatibilityPolicy &policy,
                                                                       std::uint64_t maximumPreservedBytes) const;

    private:
        struct Contents final {
            std::span<const std::byte> archive;
            std::shared_ptr<const std::vector<std::byte>> ownedArchive;
            SaveArchivePreamble preamble;
            SaveArchiveIntegrityManifest integrity;
            SaveArchiveSignatureInfo signature;
            SaveArchiveHeader header;
            SaveGameManifest manifest;
            ValidatedSaveChunkDirectory directory;
            std::shared_ptr<std::atomic<std::uint64_t>> remainingReadWork;
        };

        friend class SaveArchiveReader;
        friend class ValidatedSaveSceneCanonicalPreservation;
        friend struct SaveArchiveReaderDetail::Reader;
        explicit ValidatedSaveArchive(Contents contents) noexcept;

        std::span<const std::byte> archive_;
        std::shared_ptr<const std::vector<std::byte>> ownedArchive_;
        SaveArchivePreamble preamble_;
        SaveArchiveIntegrityManifest integrity_;
        SaveArchiveSignatureInfo signature_;
        SaveArchiveHeader header_;
        SaveGameManifest manifest_;
        ValidatedSaveChunkDirectory directory_;
        std::span<const std::byte> payload_;
        std::shared_ptr<std::atomic<std::uint64_t>> remainingReadWork_;
    };

    /** @brief Headless typed archive reader; it owns no filesystem, renderer, or gameplay callback. */
    class SaveArchiveReader final {
    public:
        /** @brief Creates a reader with finite trusted admission budgets. @param limits Trusted budgets. */
        explicit SaveArchiveReader(SaveArchiveReaderLimits limits = {});

        /**
         * @brief Admits one immutable finalized archive without decoding participant payloads.
         * @param archive Complete immutable archive bytes; the returned value borrows this span.
         * @return Complete validated archive or a deterministic typed failure.
         */
        [[nodiscard]] Result<ValidatedSaveArchive> Read(std::span<const std::byte> archive) const;

        /**
         * @brief Admits one storage-owned archive and retains its lifetime in the result.
         * @param archive Non-null immutable archive storage.
         * @return Complete validated archive or a deterministic typed failure.
         */
        [[nodiscard]] Result<ValidatedSaveArchive> Read(std::shared_ptr<const std::vector<std::byte>> archive) const;

    private:
        SaveArchiveReaderLimits limits_;
    };

    /**
     * @brief Checks that every preservable unknown source chunk survived repacking or copying unchanged.
     * @param source Integrity-verified source archive.
     * @param destination Integrity-verified candidate archive, before publication.
     * @param policy Sealed release policy used for both inspections.
     * @param maximumPreservedBytes Finite aggregate preservation budget for each archive.
     * @return Success or a typed failure naming the missing/changed owner and record.
     */
    [[nodiscard]] Result<void> VerifyUnknownDataRoundTrip(const ValidatedSaveArchive &source, const ValidatedSaveArchive &destination,
                                                          const SaveCompatibilityPolicy &policy, std::uint64_t maximumPreservedBytes);
}  // namespace Horo::Runtime
