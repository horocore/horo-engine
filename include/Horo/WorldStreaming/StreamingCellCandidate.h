#pragma once

/**
 * @file StreamingCellCandidate.h
 * @brief Worker-safe cell-header resolution and immutable candidate preparation.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/WorldStreaming/CookedWorldIndexManifest.h"
#include "Horo/WorldStreaming/StreamingCellOperation.h"

#include <compare>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    /** @brief Stable wire identity of a built-in or custom feature payload provider. */
    enum class StreamingCellProvider : std::uint16_t {
        CoreEcs = 0x0001,
        Terrain = 0x0002,
        Foliage = 0x0003,
        PhysicsMesh = 0x0004,
        Audio = 0x0005,
        NavigationMesh = 0x0006,
        Destruction = 0x0007,
        FirstCustom = 0x8000,
    };

    /** @brief Supported cell-body compression declared by the fixed header. */
    enum class StreamingCellCompression : std::uint8_t {
        None,
        Lz4,
        Zstandard,
        Count,
    };

    /** @brief Whether one provider payload is required for activation or explicitly optional. */
    enum class StreamingCellPayloadRequirement : std::uint8_t {
        Required,
        Optional,
        Count,
    };

    /** @brief Canonical fixed-header and TOC facts produced by bounded artifact parsing. */
    struct StreamingCellPayloadHeader final {
        StreamingCellProvider provider{}; /**< Stable typed provider identifier; Core ECS is the first row. */
        StreamingCellPayloadRequirement requirement{StreamingCellPayloadRequirement::Required}; /**< Activation policy. */
        std::uint32_t version{};          /**< Non-zero provider-owned payload version. */
        std::uint64_t offset{};           /**< Absolute eight-byte-aligned block offset. */
        std::uint64_t compressedSize{};   /**< Non-zero encoded block byte count. */
        std::uint64_t uncompressedSize{}; /**< Non-zero decoded block byte count. */
        std::uint32_t payloadCrc32{};     /**< CRC32 of the decoded provider payload. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingCellPayloadHeader &) const noexcept = default;
    };

    /** @brief Borrowed parsed cell header submitted for immutable preparation. */
    struct StreamingCellHeaderView final {
        static constexpr std::uint16_t CurrentMajorVersion = 1;
        static constexpr std::uint16_t CurrentMinorVersion = 0;
        static constexpr std::uint64_t FixedHeaderBytes = 96;

        std::uint16_t majorVersion{};                         /**< Exact breaking format version. */
        std::uint16_t minorVersion{};                         /**< Backward-compatible format revision. */
        StreamingCellId cell{};                               /**< Exact header cell identity. */
        StreamingCellCompression compression{};               /**< Explicit cell-body codec. */
        std::uint64_t compressedSize{};                       /**< Complete encoded cell-body bytes. */
        std::uint64_t uncompressedSize{};                     /**< Aggregate decoded provider bytes. */
        std::uint32_t payloadCrc32{};                         /**< Aggregate decoded payload CRC32. */
        Sha256Digest artifactHash{};                          /**< Canonical complete-artifact SHA-256. */
        std::span<const StreamingCellPayloadHeader> payloads; /**< Borrowed canonical TOC rows. */
    };

    /** @brief Self-contained manifest integrity facts retained by one candidate. */
    struct StreamingCellCandidateManifestRecord final {
        StreamingCellId cell{};           /**< Exact resolved manifest cell. */
        std::uint64_t uncompressedSize{}; /**< Exact aggregate decoded bytes. */
        std::uint64_t compressedSize{};   /**< Exact complete encoded body bytes. */
        std::uint32_t payloadCrc32{};     /**< Exact aggregate decoded payload CRC32. */
        Sha256Digest artifactHash{};      /**< Exact canonical artifact SHA-256. */

        [[nodiscard]] auto operator<=>(const StreamingCellCandidateManifestRecord &) const noexcept = default;
    };

    /** @brief Explicit admission state for off-owner-thread candidate preparation. */
    enum class StreamingCellCandidateLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count,
    };

    /** @brief Mandatory ceilings and exact owner evidence captured at submission. */
    struct StreamingCellCandidateContext final {
        StreamingCellOperationHandle operation{};                                        /**< Exact load operation and cell generation. */
        StreamingCellOperationKind operationKind{StreamingCellOperationKind::Load};      /**< Must identify load preparation. */
        StreamingCellOperationState operationState{StreamingCellOperationState::Queued}; /**< Must be Preparing. */
        std::uint32_t maximumPayloads{};          /**< Positive maximum TOC rows retained by a candidate. */
        std::uint32_t maximumDependencies{};      /**< Maximum manifest hard dependencies copied into a candidate. */
        std::uint64_t maximumCompressedBytes{};   /**< Positive maximum aggregate encoded bytes. */
        std::uint64_t maximumUncompressedBytes{}; /**< Positive maximum aggregate decoded bytes. */
        StreamingCellCandidateLifecycle lifecycle{StreamingCellCandidateLifecycle::Closed}; /**< Submission lifecycle evidence. */
    };

    /** @brief Generation-pinned immutable cell candidate prepared without owner-thread mutation. */
    class StreamingCellCandidate final {
    public:
        StreamingCellCandidate(const StreamingCellCandidate &) = delete;
        StreamingCellCandidate &operator=(const StreamingCellCandidate &) = delete;
        StreamingCellCandidate(StreamingCellCandidate &&) noexcept = default;
        StreamingCellCandidate &operator=(StreamingCellCandidate &&) = delete;

        /** @brief Returns the exact operation and generation fence. @return Immutable operation handle. */
        [[nodiscard]] const StreamingCellOperationHandle &Operation() const noexcept;
        /** @brief Returns the manifest-owned package chunk identity copied at preparation. @return Stable asset identity. */
        [[nodiscard]] const Assets::AssetId &ChunkAsset() const noexcept;
        /** @brief Returns self-contained manifest integrity facts copied at preparation. @return Immutable resolved record. */
        [[nodiscard]] const StreamingCellCandidateManifestRecord &ManifestEntry() const noexcept;
        /** @brief Returns the validated compression codec. @return Explicit codec selected by the artifact. */
        [[nodiscard]] StreamingCellCompression Compression() const noexcept;
        /** @brief Returns canonical owned payload rows. @return View valid until this candidate is moved from or destroyed. */
        [[nodiscard]] std::span<const StreamingCellPayloadHeader> Payloads() const noexcept;
        /** @brief Returns canonical manifest hard dependencies owned by this candidate. @return Immutable dependency view. */
        [[nodiscard]] std::span<const StreamingCellId> HardDependencies() const noexcept;

    private:
        friend Result<StreamingCellCandidate> PrepareStreamingCellCandidate(const CookedWorldIndexManifest &,
                                                                            const StreamingCellCandidateContext &,
                                                                            const StreamingCellHeaderView &);

        StreamingCellCandidate(StreamingCellOperationHandle operation, Assets::AssetId chunkAsset,
                               StreamingCellCandidateManifestRecord manifestEntry, StreamingCellCompression compression,
                               std::vector<StreamingCellPayloadHeader> payloads, std::vector<StreamingCellId> hardDependencies) noexcept;

        StreamingCellOperationHandle operation_{};
        Assets::AssetId chunkAsset_{};
        StreamingCellCandidateManifestRecord manifestEntry_{};
        StreamingCellCompression compression_{StreamingCellCompression::None};
        std::vector<StreamingCellPayloadHeader> payloads_;
        std::vector<StreamingCellId> hardDependencies_;
    };

    /**
     * @brief Resolves one manifest cell and owns a generation-pinned immutable decode candidate.
     * @param manifest Immutable topology, package-location and integrity authority retained by the caller.
     * @param context Exact load operation, generation fence, lifecycle evidence and mandatory storage ceilings.
     * @param header Borrowed parsed header/TOC facts; all retained rows are copied before success.
     * @return Owned candidate or a typed invalid, unsupported, stale, capacity or lifecycle failure.
     * @details This pure preparation boundary is safe on a worker. It performs no I/O, decode, provider calls, owner-thread
     *          transition, runtime registration or partial publication. Later owner-thread commit must revalidate Operation().
     * @throws std::bad_alloc if candidate payload storage cannot be allocated; no candidate or owner state is published.
     */
    [[nodiscard]] Result<StreamingCellCandidate> PrepareStreamingCellCandidate(const CookedWorldIndexManifest &manifest,
                                                                               const StreamingCellCandidateContext &context,
                                                                               const StreamingCellHeaderView &header);

    /**
     * @brief Parses a canonical HOROCELL artifact directly into the existing immutable candidate boundary.
     * @param manifest Immutable cell identity, package and integrity authority.
     * @param context Captured operation fence, lifecycle and mandatory byte/count ceilings.
     * @param artifact Complete borrowed standalone file or package chunk; never retained or modified.
     * @param cancellation Cooperative cancellation observed between bounded hashing and table-validation units.
     * @return Complete candidate or a typed invalid, stale, unsupported, capacity or lifecycle failure.
     * @details Verifies the canonical artifact hash, exact length, flags, TOC, padding and aggregate CRC before publication.
     *          Major versions require recooking. Encryption requires a later security adapter and is rejected here.
     *          This boundary performs no decompression, provider calls or live state changes. The owner must revalidate
     *          the retained operation fence before activation; replacement never rewrites a captured candidate.
     * @throws std::bad_alloc if bounded TOC/candidate ownership allocation fails; no candidate is published.
     */
    [[nodiscard]] Result<StreamingCellCandidate> ParseStreamingCellArtifact(const CookedWorldIndexManifest &manifest,
                                                                            const StreamingCellCandidateContext &context,
                                                                            std::span<const std::byte> artifact,
                                                                            const CancellationToken &cancellation = {});
}  // namespace Horo::WorldStreaming
