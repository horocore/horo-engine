#pragma once

/**
 * @file SaveArchiveFinalization.h
 * @brief Transactional construction and immutable finalization of save archives.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Save/SaveArchiveFraming.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /** @brief Bounded admission policy for one detached save-archive construction. */
    struct SaveArchiveFinalizationLimits final {
        SaveChunkDirectoryLimits directory;             /**< Bounds for the manifest-owned chunk directory. */
        std::uint64_t maximumArchiveBytes{4ULL << 30U}; /**< Maximum preamble, payload, and trailer bytes. */
    };

    /** @brief Immutable evidence produced after every staged chunk and archive integrity check succeeds. */
    struct SaveArchiveFinalizationSummary final {
        SaveArchiveIntegrityManifest integrity; /**< Exact finalized coverage and archive-content identity. */
        CanonicalStateHash canonicalState;      /**< Logical-state identity copied from the validated manifest. */
        std::uint64_t archiveByteLength{};      /**< Complete immutable archive length including its trailer. */
        std::uint64_t payloadByteLength{};      /**< Stored payload length excluding preamble and trailer. */
        std::size_t entryCount{};               /**< Number of finalized chunk entries. */

        [[nodiscard]] constexpr auto operator<=>(const SaveArchiveFinalizationSummary &) const noexcept = default;
    };

    /** @brief Immutable archive bytes and finalization evidence suitable for slot commit. */
    class FinalizedSaveArchive final {
    public:
        /** @brief Returns the immutable archive handle. @return Non-empty owned archive bytes. */
        [[nodiscard]] ImmutableSaveArchive Archive() const noexcept;
        /** @brief Returns the evidence calculated from the exact immutable bytes. @return Finalization summary. */
        [[nodiscard]] const SaveArchiveFinalizationSummary &Summary() const noexcept;
        /** @brief Returns the complete archive bytes without exposing mutable storage. @return Immutable byte view. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;

    private:
        FinalizedSaveArchive(std::shared_ptr<const std::vector<std::byte>> bytes, SaveArchiveFinalizationSummary summary) noexcept;

        std::shared_ptr<const std::vector<std::byte>> bytes_;
        SaveArchiveFinalizationSummary summary_;

        friend class SaveArchiveFinalizer;
    };

    /**
     * @brief Collects independently produced chunks and publishes one self-verified immutable archive.
     *
     * The finalizer owns detached staging bytes. It never exposes a partially collected archive and
     * accepts chunks only in the validated directory order, so cancellation or write failure cannot
     * publish an incomplete generation. A durable temporary copy is optional and is always addressed
     * by the exact path supplied by the caller; DiscardTemporary removes only that path.
     */
    class SaveArchiveFinalizer final {
    public:
        SaveArchiveFinalizer(const SaveArchiveFinalizer &) = delete;
        SaveArchiveFinalizer &operator=(const SaveArchiveFinalizer &) = delete;
        SaveArchiveFinalizer(SaveArchiveFinalizer &&) noexcept = default;
        SaveArchiveFinalizer &operator=(SaveArchiveFinalizer &&) noexcept = default;

        /**
         * @brief Validates metadata and directory admission before any payload allocation.
         * @param preamble Final v1 preamble bytes selected by the archive serializer.
         * @param manifest Validated save manifest describing every required chunk.
         * @param directory Manifest-owned contiguous directory in record order.
         * @param limits Trusted construction bounds.
         * @return Empty finalizer or a typed admission failure.
         */
        [[nodiscard]] static Result<SaveArchiveFinalizer> Create(std::vector<std::byte> preamble, SaveGameManifest manifest,
                                                                 SaveChunkDirectory directory,
                                                                 const SaveArchiveFinalizationLimits &limits = {});

        /**
         * @brief Appends one complete raw chunk to detached staging.
         * @param record Expected next record identity from the validated directory.
         * @param bytes Borrowed chunk bytes; never retained by the caller.
         * @return Success or a typed ordering, length, hash, budget, or allocation failure.
         */
        [[nodiscard]] Result<void> AppendChunk(SaveRecordId record, std::span<const std::byte> bytes);

        /**
         * @brief Finalizes and self-verifies the immutable archive without filesystem I/O.
         * @param trailerByteLength Explicit v1 unsigned or signed trailer size.
         * @return Immutable archive and exact summary, or a typed incomplete/integrity failure.
         */
        [[nodiscard]] Result<FinalizedSaveArchive> Finalize(std::uint32_t trailerByteLength = SaveArchiveUnsignedTrailerByteLength);

        /**
         * @brief Writes the complete finalized bytes to an operation-owned sibling temporary file.
         * @param files Durable filesystem selected by the host composition root.
         * @param temporaryPath Exact operation-owned temporary path.
         * @param trailerByteLength Explicit v1 unsigned or signed trailer size.
         * @return Immutable archive and exact summary, or the preserved durable write failure.
         */
        [[nodiscard]] Result<FinalizedSaveArchive> FinalizeTo(DurableFileSystem &files, const std::filesystem::path &temporaryPath,
                                                              std::uint32_t trailerByteLength = SaveArchiveUnsignedTrailerByteLength);

        /**
         * @brief Removes the exact temporary path after cancellation or after commit takes ownership.
         * @param files Durable filesystem selected by the host composition root.
         * @return Success, or the preserved cleanup failure.
         */
        [[nodiscard]] Result<void> DiscardTemporary(DurableFileSystem &files);

    private:
        SaveArchiveFinalizer(std::vector<std::byte> preamble, SaveGameManifest manifest, ValidatedSaveChunkDirectory directory,
                             const SaveArchiveFinalizationLimits &limits) noexcept;

        [[nodiscard]] bool FitsArchive(std::uint64_t trailerByteLength) const noexcept;

        std::vector<std::byte> preamble_;
        SaveGameManifest manifest_;
        ValidatedSaveChunkDirectory directory_;
        SaveArchiveFinalizationLimits limits_;
        std::vector<std::byte> payload_;
        std::size_t nextEntry_{};
        std::optional<std::filesystem::path> temporaryPath_;
        bool finalized_{};
    };
}  // namespace Horo::Runtime
