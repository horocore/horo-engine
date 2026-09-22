#pragma once

/**
 * @file UiRenderSnapshot.h
 * @brief Immutable backend-neutral Runtime UI render extraction values.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Runtime/Ui/UiImageResource.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <variant>

namespace Horo::Runtime::Ui {
    /** @brief Sentinel for an absent optional table reference. */
    inline constexpr std::uint32_t NoUiRenderIndex = std::numeric_limits<std::uint32_t>::max();
    inline constexpr std::uint32_t MaximumUiRenderCommands = 16'384;
    inline constexpr std::uint32_t MaximumUiRenderTextRuns = 4'096;
    inline constexpr std::uint32_t MaximumUiRenderGlyphs = 65'536;
    inline constexpr std::uint32_t MaximumUiRenderClips = 4'096;
    inline constexpr std::uint32_t MaximumUiRenderMasks = 1'024;
    inline constexpr std::uint32_t MaximumUiRenderTransforms = 4'096;
    inline constexpr std::uint32_t MaximumUiRenderResources = 4'096;
    inline constexpr std::uint32_t MaximumUiRenderSnapshotsInFlight = 64;

    struct UiRenderViewHandleTag;
    struct UiRenderSnapshotRevisionTag;
    struct UiRenderResourceRevisionTag;
    /** @brief Exact Horo-owned view incarnation; never a native surface or backend handle. */
    using UiRenderViewId = UiRuntimeHandle<UiRenderViewHandleTag>;
    /** @brief Monotonic generation of one extracted immutable render snapshot. */
    using UiRenderSnapshotRevision = UiRevision<UiRenderSnapshotRevisionTag>;
    /** @brief Exact Horo resource-source generation expected by extraction. */
    using UiRenderResourceRevision = UiRevision<UiRenderResourceRevisionTag>;

    /** @brief Finite normalized linear RGBA color. */
    struct UiLinearColor final {
        float red{};       /**< Linear red in [0, 1]. */
        float green{};     /**< Linear green in [0, 1]. */
        float blue{};      /**< Linear blue in [0, 1]. */
        float alpha{1.0F}; /**< Linear alpha in [0, 1]. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiLinearColor &) const noexcept = default;
    };

    /** @brief Semantic realization role for one stable asset reference. */
    enum class UiRenderResourceRole : std::uint8_t {
        Image,
        FontFace,
        Material,
        Mask,
    };

    /** @brief Stable Horo resource identity plus exact source generation; never a GPU handle. */
    struct UiRenderResourceReference final {
        Assets::AssetId asset;                                            /**< Stable asset identity. */
        UiRenderResourceRevision revision;                                /**< Exact immutable source generation. */
        UiRenderResourceRole role{UiRenderResourceRole::Image};           /**< Required semantic realization role. */
        UiImageColorSpace colorSpace{UiImageColorSpace::Srgb};            /**< Image sample interpretation; ignored for non-images. */
        UiImageSampling sampling;                                         /**< Image filtering semantics; ignored for non-images. */
        UiImageFallbackPolicy fallback{UiImageFallbackPolicy::Reject};    /**< Explicit missing-image policy. */
        UiImageResidencyState residency{UiImageResidencyState::Resident}; /**< Exact drawable residency evidence. */
    };

    /** @brief One positioned Horo glyph identity in logical 1/64-DIP units. */
    struct UiPositionedGlyph final {
        std::uint32_t glyph{};                           /**< Horo glyph identity, never an atlas slot. */
        std::uint32_t cluster{};                         /**< Source text cluster index. */
        UiLogicalPoint origin;                           /**< Positioned logical origin. */
        UiLogicalExtent extent;                          /**< Bounded logical glyph quad extent. */
        std::array<float, 4> uv{0.0F, 0.0F, 1.0F, 1.0F}; /**< Normalized atlas/source coordinates. */
    };

    /** @brief Immutable glyph range resolved to one exact font resource. */
    struct UiTextRun final {
        std::uint32_t fontResource{}; /**< FontFace resource table index. */
        std::uint32_t firstGlyph{};   /**< First owned glyph table index. */
        std::uint32_t glyphCount{};   /**< Bounded contiguous glyph count. */
        UiLinearColor color;          /**< Resolved linear text color. */
    };

    /** @brief Logical clip node with an optional parent intersection. */
    struct UiClip final {
        UiLogicalRect rect;                    /**< Logical intersection rectangle. */
        std::uint32_t parent{NoUiRenderIndex}; /**< Earlier parent clip or sentinel. */
    };

    /** @brief Logical mask projection backed by one typed mask resource. */
    struct UiMask final {
        UiLogicalRect rect;        /**< Logical mask bounds. */
        std::uint32_t resource{};  /**< Mask resource table index. */
        std::uint32_t transform{}; /**< Transform table index. */
    };

    /** @brief Resolved solid paint payload. */
    struct UiSolidDraw final {
        UiLinearColor color; /**< Resolved linear fill color. */
    };

    /** @brief Resolved border paint payload in logical units. */
    struct UiBorderDraw final {
        UiLinearColor color;  /**< Resolved linear border color. */
        std::int32_t width{}; /**< Non-negative 1/64-DIP width. */
    };

    /** @brief Resolved image paint payload naming one image resource. */
    struct UiImageDraw final {
        std::uint32_t resource{}; /**< Image resource table index. */
        UiLinearColor tint;       /**< Resolved linear tint. */
    };

    /** @brief Resolved sprite paint payload naming an image resource and normalized source rectangle. */
    struct UiSpriteDraw final {
        std::uint32_t resource{};                        /**< Image resource table index. */
        std::array<float, 4> uv{0.0F, 0.0F, 1.0F, 1.0F}; /**< Normalized source rectangle [u0, v0, u1, v1]. */
        UiLinearColor tint;                              /**< Resolved linear tint. */
    };

    /** @brief Resolved nine-slice paint payload naming an image resource and source borders. */
    struct UiNineSliceDraw final {
        std::uint32_t resource{};                        /**< Image resource table index. */
        std::array<float, 4> uv{0.0F, 0.0F, 1.0F, 1.0F}; /**< Normalized source rectangle [u0, v0, u1, v1]. */
        UiImageExtent sourceExtent;                      /**< Source-pixel extent represented by uv. */
        UiImageNineSliceInsets insets;                   /**< Source-pixel borders projected to the destination rect. */
        UiLinearColor tint;                              /**< Resolved linear tint. */
    };

    /** @brief Resolved text paint payload naming one positioned run. */
    struct UiTextDraw final {
        std::uint32_t run{}; /**< Text run table index. */
    };

    /** @brief Closed backend-neutral draw payload vocabulary. */
    using UiDrawPayload = std::variant<UiSolidDraw, UiBorderDraw, UiImageDraw, UiSpriteDraw, UiTextDraw, UiNineSliceDraw>;

    /** @brief One ordered logical draw emitted for an exact retained element. */
    struct UiDrawCommand final {
        UiElementHandle element;             /**< Exact resident source element. */
        UiLogicalRect rect;                  /**< Resolved logical paint bounds. */
        std::uint32_t transform{};           /**< Required transform table index. */
        std::uint32_t clip{NoUiRenderIndex}; /**< Optional clip table index. */
        std::uint32_t mask{NoUiRenderIndex}; /**< Optional mask table index. */
        float opacity{1.0F};                 /**< Resolved opacity in [0, 1]. */
        UiDrawPayload payload;               /**< Closed resolved paint payload. */
    };

    /** @brief Caller-selected capacities qualified by repository hard ceilings. */
    struct UiRenderSnapshotLimits final {
        std::uint32_t commands{};   /**< Ordered draw command ceiling. */
        std::uint32_t textRuns{};   /**< Positioned text run ceiling. */
        std::uint32_t glyphs{};     /**< Positioned glyph ceiling. */
        std::uint32_t clips{};      /**< Logical clip node ceiling. */
        std::uint32_t masks{};      /**< Logical mask ceiling. */
        std::uint32_t transforms{}; /**< Logical transform ceiling; must be positive. */
        std::uint32_t resources{};  /**< Stable resource reference ceiling. */
        /** @brief Validates every declared ceiling. @return Whether all bounds are supported. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact immutable evidence captured for one per-view extraction. */
    struct UiRenderSnapshotDescriptor final {
        RuntimeUiInstanceId instance;              /**< Exact runtime document instance. */
        UiCanvasInstanceId canvas;                 /**< Exact canvas incarnation. */
        UiDocumentId document;                     /**< Stable source document. */
        UiDocumentRevision documentRevision;       /**< Exact authored source revision. */
        UiRuntimeTreeRevision treeRevision;        /**< Exact retained tree revision. */
        UiInteractionRevision interactionRevision; /**< Exact published layout/interaction generation. */
        UiRenderSnapshotRevision snapshotRevision; /**< Exact extracted snapshot generation. */
        UiRenderViewId view;                       /**< Exact Horo view incarnation. */
        UiRenderSnapshotLimits limits;             /**< Fixed transaction bounds. */
    };

    /** @brief Non-owning complete projection submitted as one extraction transaction. */
    struct UiRenderProjection final {
        std::span<const UiDrawCommand> commands;              /**< Stable paint-order commands. */
        std::span<const UiTextRun> textRuns;                  /**< Positioned text run table. */
        std::span<const UiPositionedGlyph> glyphs;            /**< Positioned glyph table. */
        std::span<const UiClip> clips;                        /**< Logical clip table. */
        std::span<const UiMask> masks;                        /**< Logical mask table. */
        std::span<const UiLogicalTransform> transforms;       /**< Logical transform table. */
        std::span<const UiRenderResourceReference> resources; /**< Stable resource reference table. */
    };

    /** @brief Load-time bounds and exact view identity for one allocation-free extraction store. */
    struct UiRenderExtractorDescriptor final {
        UiRenderViewId view;                 /**< Exact Horo view incarnation owned by this extractor. */
        UiRenderSnapshotLimits limits;       /**< Maximum capacity reserved in every storage slot. */
        std::uint32_t concurrentSnapshots{}; /**< Simultaneously leased snapshot slots. */

        /** @brief Validates view identity, table limits, and the bounded slot count. @return Whether this store can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit admission lifecycle for one render extraction store. */
    enum class UiRenderExtractorState : std::uint8_t {
        Active,
        Closed,
    };

    class UiRenderExtractor;

    /**
     * @brief Owning immutable per-view Runtime UI render projection.
     * @details The snapshot leases one preallocated immutable slot and can outlive the tree, extractor, and caller inputs. Copies
     *          retain that exact slot until the last lease retires. Renderer may realize resources and batch equivalent paint, but
     *          cannot mutate these values or recover a live Runtime UI owner from them.
     */
    class UiRenderSnapshot final {
    public:
        /** @brief Releases this snapshot's immutable storage lease. */
        ~UiRenderSnapshot();
        /** @brief Copies one immutable snapshot and retains its exact storage slot. @param other Live snapshot to retain. */
        UiRenderSnapshot(const UiRenderSnapshot &other) noexcept;
        /** @brief Replaces this lease with a retained copy. @param other Live snapshot to retain. @return This snapshot. */
        UiRenderSnapshot &operator=(const UiRenderSnapshot &other) noexcept;
        /** @brief Transfers one immutable snapshot lease. @param other Snapshot whose lease is transferred. */
        UiRenderSnapshot(UiRenderSnapshot &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Snapshot whose lease is transferred. @return This snapshot. */
        UiRenderSnapshot &operator=(UiRenderSnapshot &&other) noexcept;

        /** @brief Returns exact extraction evidence. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiRenderSnapshotDescriptor &Descriptor() const noexcept;
        /** @brief Reports whether this object still owns a live immutable snapshot lease. @return True for a live snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns stable paint-order commands. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiDrawCommand> Commands() const noexcept;
        /** @brief Returns positioned text runs. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiTextRun> TextRuns() const noexcept;
        /** @brief Returns positioned glyph identities. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiPositionedGlyph> Glyphs() const noexcept;
        /** @brief Returns logical clip nodes. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiClip> Clips() const noexcept;
        /** @brief Returns logical mask projections. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiMask> Masks() const noexcept;
        /** @brief Returns logical transforms. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiLogicalTransform> Transforms() const noexcept;
        /** @brief Returns stable Horo resource references. @return Borrowed span owned by this snapshot. */
        [[nodiscard]] std::span<const UiRenderResourceReference> Resources() const noexcept;

    private:
        struct Storage;
        friend class UiRenderExtractor;
        /** @brief Adopts one already leased immutable storage slot. */
        explicit UiRenderSnapshot(std::shared_ptr<const Storage> storage) noexcept;
        /** @brief Retains the current slot for one additional immutable snapshot copy. */
        void Retain() const noexcept;
        /** @brief Releases the current slot and makes it reusable after the final lease. */
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /**
     * @brief Owner-thread bounded store that publishes allocation-free immutable per-view snapshots.
     * @details Creation reserves every table in every slot. Extract performs a bounded scan and never blocks, allocates fallback
     *          storage, or overwrites a leased slot. Close stops admission while existing snapshots remain valid and drain normally.
     */
    class UiRenderExtractor final {
    public:
        /**
         * @brief Allocates all storage required by one exact view before frame-hot extraction begins.
         * @param descriptor Exact view, per-slot table limits, and simultaneous snapshot bound.
         * @return Active extractor or a typed identity/capacity failure.
         * @pre Called exactly once by the Runtime UI view owner for descriptor.view.
         */
        [[nodiscard]] static Result<UiRenderExtractor> Create(const UiRenderExtractorDescriptor &descriptor);
        /** @brief Closes extraction admission; outstanding snapshots keep their slots alive. */
        ~UiRenderExtractor();
        /** @brief Transfers an extraction store. @param other Extractor whose ownership is transferred. */
        UiRenderExtractor(UiRenderExtractor &&) noexcept;
        /** @brief Replaces this store by transfer. @param other Extractor whose ownership is transferred. @return This extractor. */
        UiRenderExtractor &operator=(UiRenderExtractor &&) noexcept;
        UiRenderExtractor(const UiRenderExtractor &) = delete;
        UiRenderExtractor &operator=(const UiRenderExtractor &) = delete;

        /**
         * @brief Validates and copies one complete ordered extraction transaction into a free preallocated slot.
         * @param tree Exact active retained tree supplying identity and residency evidence.
         * @param descriptor Exact view, source revisions, strictly increasing output revision, and requested bounds.
         * @param projection Complete non-owning projection copied by the transaction.
         * @return Complete snapshot lease or a typed validation, exhaustion, capacity, or lifecycle failure.
         * @pre Calls for one extractor are serialized on its owner thread.
         */
        [[nodiscard]] Result<UiRenderSnapshot> Extract(const UiElementTree &tree, const UiRenderSnapshotDescriptor &descriptor,
                                                       const UiRenderProjection &projection);
        /** @brief Idempotently stops new extraction without invalidating live snapshot leases. */
        void Close() noexcept;
        /** @brief Reports whether no snapshot slot is currently leased. @return True after every snapshot copy retires. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns the current admission lifecycle. @return Active or Closed. */
        [[nodiscard]] UiRenderExtractorState State() const noexcept;

    private:
        struct Storage;
        explicit UiRenderExtractor(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
