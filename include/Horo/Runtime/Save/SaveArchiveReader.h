#pragma once

/**
 * @file SaveArchiveReader.h
 * @brief Backend-neutral bounded admission for finalized runtime-save archives.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveArchiveFraming.h"

#include <array>
#include <cstddef>
#include <cstdint>
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

    /** @brief Finite budgets applied to every untrusted archive admission path. */
    struct SaveArchiveReaderLimits final {
        std::size_t maximumArchiveBytes{4ULL << 30U};
        std::uint64_t maximumStoredPayloadBytes{4ULL << 30U};
        std::uint64_t maximumDecodedBytes{64ULL << 20U};
        std::size_t maximumEntries{16'384};
        std::size_t maximumNestingDepth{8};
        std::uint64_t maximumExpansionRatio{64};
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
     * overload when the storage backend owns the archive; the returned value then retains that
     * ownership until all selected chunk spans and metadata references are destroyed.
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
         * @return Borrowed chunk bytes or an empty optional for an unknown lookup.
         */
        [[nodiscard]] Result<std::optional<std::span<const std::byte>>> SelectChunk(SaveRecordId record) const;

    private:
        friend class SaveArchiveReader;
        friend struct SaveArchiveReaderDetail::Reader;
        ValidatedSaveArchive(std::span<const std::byte> archive, std::shared_ptr<const std::vector<std::byte>> ownedArchive,
                             SaveArchivePreamble preamble, SaveArchiveIntegrityManifest integrity, SaveArchiveSignatureInfo signature,
                             SaveArchiveHeader header, SaveGameManifest manifest, ValidatedSaveChunkDirectory directory) noexcept;

        std::span<const std::byte> archive_;
        std::shared_ptr<const std::vector<std::byte>> ownedArchive_;
        SaveArchivePreamble preamble_;
        SaveArchiveIntegrityManifest integrity_;
        SaveArchiveSignatureInfo signature_;
        SaveArchiveHeader header_;
        SaveGameManifest manifest_;
        ValidatedSaveChunkDirectory directory_;
        std::span<const std::byte> payload_;
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
}  // namespace Horo::Runtime
