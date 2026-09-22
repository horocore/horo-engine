#pragma once

/**
 * @file UiGlyphAtlas.h
 * @brief Bounded backend-neutral Runtime UI glyph atlas residency and upload contracts.
 */

#include "Horo/Runtime/Ui/UiTextLayout.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiGlyphAtlasPages = 64;
    inline constexpr std::uint32_t MaximumUiGlyphAtlasTilesPerPage = 4'096;
    inline constexpr std::uint32_t MaximumUiGlyphAtlasUploads = 1'024;
    inline constexpr std::uint32_t MaximumUiGlyphAtlasFramesInFlight = 8;
    inline constexpr std::uint32_t MaximumUiGlyphAtlasUsesPerFrame = 16'384;
    inline constexpr std::uint32_t MaximumUiGlyphAtlasEvictionsPerRequest = 64;
    inline constexpr std::size_t MaximumUiGlyphAtlasPageBytes = 16U * 1024U * 1024U;
    inline constexpr std::size_t MaximumUiGlyphAtlasPendingUploadBytes = 32U * 1024U * 1024U;

    struct UiGlyphAtlasRevisionTag;
    struct UiGlyphAtlasPageHandleTag;
    struct UiGlyphAtlasEntryHandleTag;
    struct UiGlyphAtlasUploadHandleTag;
    struct UiGlyphAtlasFrameHandleTag;

    /** @brief Monotonic publication revision of one logical atlas generation. */
    using UiGlyphAtlasRevision = UiRevision<UiGlyphAtlasRevisionTag>;
    /** @brief Generation-safe identity of one logical atlas page. */
    using UiGlyphAtlasPageId = UiRuntimeHandle<UiGlyphAtlasPageHandleTag>;
    /** @brief Generation-safe identity of one resident or pending glyph entry. */
    using UiGlyphAtlasEntryId = UiRuntimeHandle<UiGlyphAtlasEntryHandleTag>;
    /** @brief Generation-safe identity of one copied glyph upload request. */
    using UiGlyphAtlasUploadId = UiRuntimeHandle<UiGlyphAtlasUploadHandleTag>;
    /** @brief Generation-safe identity of one frame pin set. */
    using UiGlyphAtlasFrameId = UiRuntimeHandle<UiGlyphAtlasFrameHandleTag>;

    /** @brief Pixel encoding supplied by the bounded rasterization owner. */
    enum class UiGlyphAtlasRasterFormat : std::uint8_t {
        Alpha8,
        Rgba8,
        Count,
    };

    /** @brief Lifecycle state of one upload request. */
    enum class UiGlyphAtlasUploadState : std::uint8_t {
        Pending,
        Submitted,
        Ready,
        Failed,
        Cancelled,
        Retired,
        Count,
    };

    /** @brief Resolution result returned to render extraction for one requested glyph. */
    enum class UiGlyphAtlasResolutionState : std::uint8_t {
        Resident,
        Fallback,
        FallbackPending,
        Count,
    };

    /** @brief Completion outcome used when a renderer releases one frame's atlas pins. */
    enum class UiGlyphAtlasFrameOutcome : std::uint8_t {
        Presented,
        Skipped,
        Failed,
        DeviceLost,
        Count,
    };

    /** @brief Explicit reason for replacing all physical atlas residency. */
    enum class UiGlyphAtlasResetReason : std::uint8_t {
        Reload,
        DeviceLost,
        Count,
    };

    /**
     * @brief Backend-neutral completion point for one submitted atlas upload.
     * @details The renderer maps its private timeline or command-buffer evidence to this bounded value. No native handle crosses
     *          the Runtime UI boundary.
     */
    struct UiGlyphAtlasCompletionPoint final {
        std::uint32_t queue{}; /**< Non-zero logical queue identity. */
        std::uint64_t value{}; /**< Strictly positive queue point. */

        /** @brief Checks that the completion evidence is representable. @return Whether the point is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return queue != 0 && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiGlyphAtlasCompletionPoint &) const noexcept = default;
    };

    /** @brief Raster variant identity used to keep different scale realizations separate. */
    struct UiGlyphAtlasVariant final {
        UiTextScale scale{}; /**< Logical text scale used by rasterization. */

        /** @brief Checks the fixed-point scale. @return Whether this variant is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiGlyphAtlasVariant &) const noexcept = default;
    };

    /** @brief Stable face and glyph identity used for atlas lookup; never a page or GPU slot. */
    struct UiGlyphAtlasGlyphKey final {
        UiTextFaceId face;           /**< Immutable Runtime UI face identity. */
        std::uint32_t glyph{};       /**< Backend-neutral glyph identity. */
        UiGlyphAtlasVariant variant; /**< Raster scale variant. */

        /** @brief Checks face, glyph, and variant identity. @return Whether the key is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiGlyphAtlasGlyphKey &) const noexcept = default;
    };

    /** @brief Integer page-space extent of one atlas page. */
    struct UiGlyphAtlasPageExtent final {
        std::uint32_t width{};  /**< Page width in raster pixels. */
        std::uint32_t height{}; /**< Page height in raster pixels. */

        /** @brief Checks non-zero page dimensions. @return Whether the extent is representable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiGlyphAtlasPageExtent &) const noexcept = default;
    };

    /** @brief Fixed tile extent used by the bounded page allocator. */
    struct UiGlyphAtlasTileExtent final {
        std::uint32_t width{};  /**< Tile width in raster pixels. */
        std::uint32_t height{}; /**< Tile height in raster pixels. */

        /** @brief Checks non-zero tile dimensions. @return Whether the extent is representable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiGlyphAtlasTileExtent &) const noexcept = default;
    };

    /** @brief Pixel-space rectangle and normalized sampling coordinates for one atlas entry. */
    struct UiGlyphAtlasRect final {
        std::uint32_t x{};      /**< Inclusive page-space x coordinate. */
        std::uint32_t y{};      /**< Inclusive page-space y coordinate. */
        std::uint32_t width{};  /**< Raster width occupied by the glyph. */
        std::uint32_t height{}; /**< Raster height occupied by the glyph. */

        /** @brief Checks a non-empty rectangle without assuming a page extent. @return Whether the rectangle is representable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const UiGlyphAtlasRect &) const noexcept = default;
    };

    /** @brief Stable page placement shared with a renderer without exposing a native image handle. */
    struct UiGlyphAtlasPlacement final {
        UiGlyphAtlasPageId page;                         /**< Exact page generation containing the entry. */
        UiGlyphAtlasRect pixels;                         /**< Destination rectangle in page pixels. */
        std::array<float, 4> uv{0.0F, 0.0F, 1.0F, 1.0F}; /**< Normalized sampling rectangle. */

        /** @brief Checks page identity, rectangle, and normalized coordinates. @return Whether placement evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Finite metadata and staging bounds reserved before frame-hot atlas work. */
    struct UiGlyphAtlasLimits final {
        std::uint32_t pages{4};
        std::uint32_t maximumUploads{256};
        std::size_t maximumPendingUploadBytes{4U * 1024U * 1024U};
        std::uint32_t maximumFramesInFlight{3};
        std::uint32_t maximumUsesPerFrame{4'096};
        std::uint32_t maximumEvictionsPerRequest{1};

        /** @brief Checks every finite bound against repository hard ceilings. @return Whether limits are supported. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return pages > 0 && pages <= MaximumUiGlyphAtlasPages && maximumUploads > 0 && maximumUploads <= MaximumUiGlyphAtlasUploads &&
                   maximumPendingUploadBytes > 0 && maximumPendingUploadBytes <= MaximumUiGlyphAtlasPendingUploadBytes &&
                   maximumFramesInFlight > 0 && maximumFramesInFlight <= MaximumUiGlyphAtlasFramesInFlight && maximumUsesPerFrame > 0 &&
                   maximumUsesPerFrame <= MaximumUiGlyphAtlasUsesPerFrame && maximumEvictionsPerRequest > 0 &&
                   maximumEvictionsPerRequest <= MaximumUiGlyphAtlasEvictionsPerRequest;
        }
    };

    /** @brief Complete load-time policy for one Runtime UI glyph atlas owner. */
    struct UiGlyphAtlasDescriptor final {
        UiOwnershipGeneration ownership;                                   /**< Exact Runtime UI owner generation. */
        UiGlyphAtlasPageExtent pageExtent;                                 /**< Fixed extent of every logical page. */
        UiGlyphAtlasTileExtent tileExtent;                                 /**< Fixed tile bound for every raster upload. */
        UiGlyphAtlasRasterFormat format{UiGlyphAtlasRasterFormat::Alpha8}; /**< Page pixel encoding. */
        UiGlyphAtlasGlyphKey fallback;                                     /**< Required terminal fallback glyph identity. */
        UiGlyphAtlasLimits limits;                                         /**< Fixed page, upload, frame, and eviction bounds. */
        UiGlyphAtlasRevision initialRevision;                              /**< First logical residency revision. */

        /** @brief Validates owner identity, page tiling, format, fallback, and finite capacities. @return Whether creation is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Bounded raster payload copied by upload admission. */
    struct UiGlyphAtlasRasterData final {
        UiGlyphAtlasGlyphKey key;                                          /**< Stable face/glyph/variant identity. */
        UiGlyphAtlasRasterFormat format{UiGlyphAtlasRasterFormat::Alpha8}; /**< Pixel encoding. */
        std::uint32_t width{};                                             /**< Raster width in pixels. */
        std::uint32_t height{};                                            /**< Raster height in pixels. */
        std::uint32_t rowBytes{};                                          /**< Bytes between consecutive rows. */
        std::span<const std::byte> bytes;                                  /**< Caller-owned bytes valid for admission only. */

        /** @brief Validates row stride, payload size, and non-empty raster evidence. @return Whether the payload is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable upload destination and source evidence returned to the renderer adapter. */
    struct UiGlyphAtlasUploadDescriptor final {
        UiGlyphAtlasUploadId upload;                                       /**< Exact request identity. */
        UiGlyphAtlasEntryId entry;                                         /**< Entry reserved by this upload. */
        UiGlyphAtlasGlyphKey key;                                          /**< Stable glyph identity being realized. */
        UiGlyphAtlasPlacement placement;                                   /**< Page destination and UV evidence. */
        UiGlyphAtlasRevision revision;                                     /**< Atlas revision that admitted the request. */
        UiGlyphAtlasRasterFormat format{UiGlyphAtlasRasterFormat::Alpha8}; /**< Pixel encoding. */
        std::uint32_t rowBytes{};                                          /**< Copied source row stride. */
        std::size_t byteCount{};                                           /**< Copied source byte count. */
        bool fallback{};                                                   /**< Whether completion publishes the terminal fallback entry. */

        /** @brief Checks all copied upload metadata. @return Whether the descriptor is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable glyph lookup result consumed by render extraction. */
    struct UiGlyphAtlasLookup final {
        UiGlyphAtlasGlyphKey requested;                                           /**< Glyph requested by the text run. */
        UiGlyphAtlasGlyphKey resolved;                                            /**< Resident glyph actually sampled. */
        UiGlyphAtlasEntryId entry;                                                /**< Exact resident entry pinned by the frame. */
        UiGlyphAtlasPlacement placement;                                          /**< Page/UV evidence for sampling. */
        UiGlyphAtlasRevision revision;                                            /**< Exact atlas revision used by the lookup. */
        UiGlyphAtlasResolutionState state{UiGlyphAtlasResolutionState::Fallback}; /**< Whether requested or fallback glyph was used. */

        /** @brief Checks the complete generation-correlated lookup. @return Whether the lookup can be submitted. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Bounded eviction request. Eviction never removes fallback or frame-pinned entries. */
    struct UiGlyphAtlasEvictionRequest final {
        std::uint32_t maximumEntries{}; /**< Maximum entries this call may evict. */

        /** @brief Checks a positive bounded eviction count. @return Whether the request is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumEntries > 0 && maximumEntries <= MaximumUiGlyphAtlasEvictionsPerRequest;
        }
    };

    /** @brief Evidence from one bounded eviction pass. */
    struct UiGlyphAtlasEvictionReport final {
        std::uint32_t evictedEntries{};  /**< Entries removed from residency. */
        std::uint32_t pinnedEntries{};   /**< Resident entries protected by in-flight frames. */
        std::uint32_t residentEntries{}; /**< Resident entries remaining after the pass. */
    };

    /** @brief Evidence from one reload or device-loss generation replacement. */
    struct UiGlyphAtlasResetReport final {
        UiGlyphAtlasResetReason reason{UiGlyphAtlasResetReason::Reload}; /**< Explicit reset cause. */
        UiGlyphAtlasRevision previousRevision;                           /**< Revision invalidated by the reset. */
        UiGlyphAtlasRevision revision;                                   /**< New revision admitting future uploads. */
        std::uint32_t invalidatedEntries{};                              /**< Resident entries requiring re-upload. */
        std::uint32_t cancelledUploads{};                                /**< Pending uploads discarded at the boundary. */
        bool requiresFallbackUpload{};                                   /**< True until the terminal fallback is uploaded again. */
    };

    /** @brief Bounded owner and staging state observable without exposing implementation storage. */
    struct UiGlyphAtlasSnapshot final {
        UiGlyphAtlasRevision revision;    /**< Current logical atlas revision. */
        std::uint32_t residentEntries{};  /**< Ready entries available for lookup. */
        std::uint32_t pendingEntries{};   /**< Entries waiting for upload completion. */
        std::uint32_t pinnedEntries{};    /**< Distinct resident entries protected by frames. */
        std::uint32_t activeFrames{};     /**< Frames whose pins have not retired. */
        std::uint32_t pendingUploads{};   /**< Pending or submitted upload records. */
        std::size_t pendingUploadBytes{}; /**< Staging bytes retained by pending/submitted work. */
        std::uint64_t evictionCount{};    /**< Total successful bounded evictions. */
        std::uint64_t pressureCount{};    /**< Requests rejected because no safe tile was available. */
        bool acceptingRequests{};         /**< False after StopAdmission or Shutdown. */
    };

    /** @brief Explicit lifecycle state of one glyph atlas owner. */
    enum class UiGlyphAtlasState : std::uint8_t {
        Active,
        Closed,
        Count,
    };

    /**
     * @brief Owns bounded logical glyph-page residency and renderer-neutral upload state.
     * @details The atlas never rasterizes, performs I/O, selects a font fallback, creates GPU objects, or exposes native
     *          handles. Raster bytes are copied at admission; the renderer adapter consumes the immutable upload descriptor and
     *          payload, then publishes typed completion. Every lookup pins its entry to one frame until RetireFrame, so eviction
     *          cannot invalidate a glyph used by an in-flight frame.
     */
    class UiGlyphAtlas final {
    public:
        /**
         * @brief Creates an atlas with all page, entry, frame, upload, and staging storage reserved.
         * @param descriptor Fixed owner, page geometry, fallback identity, and work bounds.
         * @return Active atlas or a typed invalid/capacity/allocation failure.
         */
        [[nodiscard]] static Result<UiGlyphAtlas> Create(const UiGlyphAtlasDescriptor &descriptor);
        /** @brief Closes admission and releases CPU storage after the owner is destroyed. */
        ~UiGlyphAtlas();
        /** @brief Transfers one atlas owner. @param other Atlas owner to transfer. */
        UiGlyphAtlas(UiGlyphAtlas &&other) noexcept;
        /** @brief Replaces this atlas by transferred ownership. @param other Atlas owner to transfer. @return This atlas. */
        UiGlyphAtlas &operator=(UiGlyphAtlas &&other) noexcept;
        UiGlyphAtlas(const UiGlyphAtlas &) = delete;
        UiGlyphAtlas &operator=(const UiGlyphAtlas &) = delete;

        /** @brief Returns the fixed logical page generations available to the renderer adapter. */
        [[nodiscard]] std::span<const UiGlyphAtlasPageId> Pages() const noexcept;
        /** @brief Returns the fixed page extent used by every page. */
        [[nodiscard]] UiGlyphAtlasPageExtent PageExtent() const noexcept;
        /** @brief Returns the fixed tile extent accepted by uploads. */
        [[nodiscard]] UiGlyphAtlasTileExtent TileExtent() const noexcept;
        /** @brief Returns the admitted page pixel format. */
        [[nodiscard]] UiGlyphAtlasRasterFormat Format() const noexcept;
        /** @brief Returns the current logical atlas revision. */
        [[nodiscard]] UiGlyphAtlasRevision Revision() const noexcept;

        /**
         * @brief Copies one bounded raster payload into a preallocated upload record.
         * @param raster Stable glyph identity, tile-sized raster metadata, and caller-owned bytes.
         * @return Pending upload identity or typed validation, duplicate, pressure, staging, or lifecycle failure.
         * @pre Calls for one atlas are serialized on its Runtime UI owner thread.
         */
        [[nodiscard]] Result<UiGlyphAtlasUploadId> RequestUpload(const UiGlyphAtlasRasterData &raster);

        /** @brief Alias for RequestUpload used by upload-producing adapters. */
        [[nodiscard]] Result<UiGlyphAtlasUploadId> QueueUpload(const UiGlyphAtlasRasterData &raster) {
            return RequestUpload(raster);
        }

        /** @brief Returns immutable upload destination metadata while the request remains tracked. */
        [[nodiscard]] Result<UiGlyphAtlasUploadDescriptor> DescribeUpload(UiGlyphAtlasUploadId upload) const;
        /** @brief Returns copied bytes for a pending upload before renderer submission. */
        [[nodiscard]] Result<std::span<const std::byte>> Payload(UiGlyphAtlasUploadId upload) const;
        /** @brief Returns the current state of one upload record. */
        [[nodiscard]] Result<UiGlyphAtlasUploadState> State(UiGlyphAtlasUploadId upload) const;
        /** @brief Records backend-neutral completion evidence after native submission succeeds. */
        [[nodiscard]] Result<void> MarkSubmitted(UiGlyphAtlasUploadId upload, UiGlyphAtlasCompletionPoint completion);
        /** @brief Publishes one submitted glyph as resident and releases its staging charge. */
        [[nodiscard]] Result<void> Complete(UiGlyphAtlasUploadId upload);
        /** @brief Publishes an original typed renderer failure and releases or retains staging as appropriate. */
        [[nodiscard]] Result<void> Fail(UiGlyphAtlasUploadId upload, const Error &error);
        /** @brief Cancels pending or submitted work; submitted bytes remain charged until Retire. */
        [[nodiscard]] Result<void> Cancel(UiGlyphAtlasUploadId upload);
        /** @brief Retires cancelled work after the renderer proves that submitted work is no longer referenced. */
        [[nodiscard]] Result<void> Retire(UiGlyphAtlasUploadId upload);
        /** @brief Removes a terminal upload record after its entry and staging ownership are settled. */
        [[nodiscard]] Result<void> Discard(UiGlyphAtlasUploadId upload);

        /** @brief Starts one bounded frame pin set. @return Frame identity or in-flight capacity failure. */
        [[nodiscard]] Result<UiGlyphAtlasFrameId> BeginFrame();
        /**
         * @brief Resolves and pins a glyph for one active frame.
         * @param frame Active frame receiving the pin.
         * @param requested Stable face/glyph/variant identity from text extraction.
         * @return Resident glyph lookup, fallback lookup, or typed frame/fallback failure.
         */
        [[nodiscard]] Result<UiGlyphAtlasLookup> Resolve(UiGlyphAtlasFrameId frame, const UiGlyphAtlasGlyphKey &requested);
        /** @brief Releases every distinct entry pinned by a frame after presentation, skip, failure, or device loss. */
        [[nodiscard]] Result<void> RetireFrame(UiGlyphAtlasFrameId frame, UiGlyphAtlasFrameOutcome outcome);

        /** @brief Evicts at most the requested number of safe least-recently-used entries. */
        [[nodiscard]] Result<UiGlyphAtlasEvictionReport> Evict(const UiGlyphAtlasEvictionRequest &request);
        /**
         * @brief Replaces the logical atlas generation after reload or device loss.
         * @details The owner must first retire all frames and submitted uploads. Existing page, entry, upload, and lookup
         *          identities become stale; future glyphs must be uploaded again, including the fallback glyph.
         */
        [[nodiscard]] Result<UiGlyphAtlasResetReport> Reset(UiGlyphAtlasResetReason reason);

        /** @brief Returns bounded residency, pressure, frame, and staging accounting. */
        [[nodiscard]] UiGlyphAtlasSnapshot Snapshot() const noexcept;
        /** @brief Stops new upload and frame admission while existing terminal retirement remains valid. */
        void StopAdmission() noexcept;

        /** @brief Idempotently closes admission and cancels not-yet-submitted uploads. */
        void Shutdown() noexcept {
            StopAdmission();
        }

        /** @brief Reports whether no frame or submitted upload still retains atlas ownership. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns the explicit admission state. */
        [[nodiscard]] UiGlyphAtlasState State() const noexcept;

    private:
        struct Storage;
        explicit UiGlyphAtlas(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
