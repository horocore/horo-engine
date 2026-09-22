#pragma once

/**
 * @file UiTextLayout.h
 * @brief Bounded Runtime UI text measurement, line layout, and overflow contracts.
 */

#include "Horo/Runtime/Ui/UiLayout.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint32_t MaximumUiTextLayoutSourceBytes = 1U * 1024U * 1024U;
    inline constexpr std::uint32_t MaximumUiTextLayoutClusters = 65'536;
    inline constexpr std::uint32_t MaximumUiTextLayoutGlyphs = 65'536;
    inline constexpr std::uint32_t MaximumUiTextLayoutLines = 4'096;
    inline constexpr std::uint32_t MaximumUiTextLayoutRuns = 4'096;
    inline constexpr std::uint32_t MaximumUiTextLayoutResultsInFlight = 8;
    inline constexpr std::uint32_t MaximumUiTextLayoutScale = 16U * 1024U;
    inline constexpr std::uint32_t UiTextLayoutScaleUnit = 1024U;
    inline constexpr std::uint32_t NoUiTextLayoutCluster = 0xFFFF'FFFFU;

    struct UiTextLayoutFaceIdentityTag;
    struct UiTextLayoutRevisionTag;

    /** @brief Stable face identity carried from the shaping/font domain to text layout. */
    using UiTextFaceId = UiStableId<UiTextLayoutFaceIdentityTag>;
    /** @brief Monotonic immutable publication revision of one text layout result. */
    using UiTextLayoutRevision = UiRevision<UiTextLayoutRevisionTag>;

    /** @brief Paragraph direction used to resolve leading and trailing alignment. */
    enum class UiTextFlowDirection : std::uint8_t {
        LeftToRight,
        RightToLeft,
        Count,
    };

    /** @brief Closed line-wrapping policy applied only at shaped-cluster boundaries. */
    enum class UiTextWrapMode : std::uint8_t {
        NoWrap,
        Word,
        Character,
        Count,
    };

    /** @brief Closed paint/overflow policy for text exceeding its assigned content box. */
    enum class UiTextOverflowMode : std::uint8_t {
        Visible,
        Clip,
        Ellipsis,
        Count,
    };

    /** @brief Horizontal line alignment inside the assigned logical content width. */
    enum class UiTextHorizontalAlignment : std::uint8_t {
        Leading,
        Center,
        Trailing,
        Justify,
        Count,
    };

    /** @brief Vertical block alignment inside the assigned logical content height. */
    enum class UiTextVerticalAlignment : std::uint8_t {
        Top,
        Center,
        Bottom,
        Count,
    };

    /** @brief Break opportunity exposed by the upstream Unicode line-break/shaping stage. */
    enum class UiTextBreakOpportunity : std::uint8_t {
        None,
        Optional,
        Mandatory,
        Count,
    };

    /**
     * @brief Positive fixed-point text scale in 1/1024 units.
     * @details Scaling is applied with checked ties-to-even rounding in the logical 1/64-DIP domain. A renderer or device DPI
     *          value is never queried by this type.
     */
    struct UiTextScale final {
        std::uint32_t value{UiTextLayoutScaleUnit}; /**< Positive scale in 1/1024 units. */

        /**
         * @brief Creates a bounded positive scale.
         * @param value Scale in 1/1024 units.
         * @return Valid scale or UiErrors::TextLayoutInputInvalid.
         */
        [[nodiscard]] static Result<UiTextScale> Create(std::uint32_t value);
        /** @brief Checks positivity and the hard scale bound. @return Whether the scale is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextScale &) const noexcept = default;
    };

    /** @brief Options shared by intrinsic measurement, wrapping, arranging, and render extraction. */
    struct UiTextLayoutOptions final {
        UiTextWrapMode wrap{UiTextWrapMode::Word};                                /**< Cluster wrapping policy. */
        UiTextOverflowMode overflow{UiTextOverflowMode::Visible};                 /**< Overflow/paint policy. */
        UiTextHorizontalAlignment horizontal{UiTextHorizontalAlignment::Leading}; /**< Horizontal alignment. */
        UiTextVerticalAlignment vertical{UiTextVerticalAlignment::Top};           /**< Vertical alignment. */
        UiTextFlowDirection direction{UiTextFlowDirection::LeftToRight};          /**< Paragraph direction. */
        UiTextScale scale{};                                                      /**< Logical text scale. */
        std::int32_t lineHeight{}; /**< Unscaled line height, or zero for shaped natural metrics. */
        std::uint32_t maxLines{};  /**< Maximum lines, or zero for no authored line limit. */

        /** @brief Checks closed policies, scale, line height, and global line bounds. @return Whether options are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutOptions &) const noexcept = default;
    };

    /** @brief One shaped glyph supplied by the Unicode shaping/font domain. */
    struct UiTextShapedGlyph final {
        std::uint32_t glyph{};  /**< Backend-neutral glyph identity. */
        UiLogicalPoint offset;  /**< Offset from the cluster pen in logical units. */
        UiLogicalPoint advance; /**< Non-negative horizontal logical advance. */

        /** @brief Checks finite logical glyph placement evidence. @return Whether the glyph can enter layout. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextShapedGlyph &) const noexcept = default;
    };

    /** @brief Face-selected contiguous glyph range produced by shaping. */
    struct UiTextShapedRun final {
        UiTextFaceId face;                                               /**< Stable immutable face identity. */
        std::uint32_t firstGlyph{};                                      /**< First glyph in the shaped glyph table. */
        std::uint32_t glyphCount{};                                      /**< Number of glyphs in this run. */
        UiTextFlowDirection direction{UiTextFlowDirection::LeftToRight}; /**< Resolved run direction. */

        /** @brief Checks face, direction, and glyph range representation. @return Whether the run is valid. */
        [[nodiscard]] bool IsValid(std::size_t glyphCountLimit) const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextShapedRun &) const noexcept = default;
    };

    /** @brief Source grapheme/line-break cluster used as the indivisible wrapping unit. */
    struct UiTextShapedCluster final {
        std::uint32_t byteStart{};                                             /**< Inclusive UTF-8 byte offset. */
        std::uint32_t byteEnd{};                                               /**< Exclusive UTF-8 byte offset. */
        std::uint32_t firstGlyph{};                                            /**< First glyph or NoUiTextLayoutCluster when empty. */
        std::uint32_t glyphCount{};                                            /**< Number of glyphs attributed to this cluster. */
        UiLogicalPoint advance;                                                /**< Cluster advance used for wrapping. */
        UiTextBreakOpportunity breakOpportunity{UiTextBreakOpportunity::None}; /**< Break after this cluster. */

        /**
         * @brief Checks byte ordering, glyph range, advance, and break evidence.
         * @param sourceBytes Total UTF-8 source byte count.
         * @param glyphCountLimit Number of supplied shaped glyphs.
         * @return Whether the cluster is valid.
         */
        [[nodiscard]] bool IsValid(std::size_t sourceBytes, std::size_t glyphCountLimit) const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextShapedCluster &) const noexcept = default;
    };

    /** @brief Font metrics copied from one immutable shaped-text result. */
    struct UiTextShapedMetrics final {
        std::int32_t ascent{};  /**< Non-negative ascent in logical units. */
        std::int32_t descent{}; /**< Non-negative descent magnitude in logical units. */
        std::int32_t lineGap{}; /**< Non-negative line gap in logical units. */

        /** @brief Checks metric signs and checked natural line-height arithmetic. @return Whether metrics are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextShapedMetrics &) const noexcept = default;
    };

    /** @brief Caller-selected finite capacities reserved by one text-layout owner. */
    struct UiTextLayoutLimits final {
        std::uint32_t sourceBytes{MaximumUiTextLayoutSourceBytes}; /**< Maximum source byte evidence. */
        std::uint32_t clusters{MaximumUiTextLayoutClusters};       /**< Maximum shaped clusters. */
        std::uint32_t glyphs{MaximumUiTextLayoutGlyphs};           /**< Maximum shaped/output glyphs. */
        std::uint32_t lines{MaximumUiTextLayoutLines};             /**< Maximum output lines. */
        std::uint32_t runs{MaximumUiTextLayoutRuns};               /**< Maximum output face runs. */

        /** @brief Checks every positive capacity against the repository hard bounds. @return Whether limits are supported. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutLimits &) const noexcept = default;
    };

    /**
     * @brief Non-owning shaped text evidence consumed by one synchronous layout call.
     * @details The caller owns all spans for the duration of the call. Layout copies only positioned output into an immutable
     *          result lease; it never retains source text, font objects, renderer handles, or callbacks.
     */
    struct UiTextShapedTextView final {
        UiLayoutContentRevision content;               /**< Exact resolved text/content revision. */
        std::uint32_t sourceBytes{};                   /**< Total UTF-8 source byte count. */
        UiTextShapedMetrics metrics;                   /**< Immutable font metrics for this shaped result. */
        std::span<const UiTextShapedRun> runs;         /**< Face-selected shaped runs. */
        std::span<const UiTextShapedGlyph> glyphs;     /**< Shaped glyph table. */
        std::span<const UiTextShapedCluster> clusters; /**< Ordered grapheme/line-break clusters. */

        /**
         * @brief Validates all ranges, coverage, source bytes, and bounded shaped evidence.
         * @param limits Capacities reserved by the receiving layout engine.
         * @return Whether the view can be consumed without fallback work or allocation.
         */
        [[nodiscard]] bool IsValid(const UiTextLayoutLimits &limits) const noexcept;
    };

    /** @brief Exact owner, tree, content, font/intrinsic, and policy lineage for one text candidate. */
    struct UiTextLayoutSource final {
        RuntimeUiInstanceId instance;      /**< Exact runtime UI instance. */
        UiCanvasInstanceId canvas;         /**< Exact canvas incarnation. */
        UiElementHandle element;           /**< Exact retained text element. */
        UiDocumentId document;             /**< Stable source document. */
        UiLayoutSourceRevisions revisions; /**< Complete layout source revisions. */

        /** @brief Checks ownership and complete source revision evidence. @return Whether the source is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutSource &) const noexcept = default;
    };

    /** @brief Complete non-owning input for one bounded measure/arrange transaction. */
    struct UiTextLayoutRequest final {
        UiTextLayoutSource source;                    /**< Exact owner and revision evidence. */
        UiLogicalRect assignedContent;                /**< Definite logical content box used for wrapping/alignment. */
        UiLayoutConstraints constraints;              /**< Intrinsic result clamp range. */
        UiTextLayoutOptions options;                  /**< Wrapping, alignment, overflow, and scale policy. */
        UiTextShapedTextView shaped;                  /**< Main shaped text view. */
        std::optional<UiTextShapedTextView> ellipsis; /**< Pre-shaped ellipsis view, required only when ellipsis is needed. */

        /**
         * @brief Validates the complete candidate against fixed owner capacities.
         * @param limits Capacities reserved by the receiving engine.
         * @return Whether the request may enter the frame-hot algorithm.
         */
        [[nodiscard]] bool IsValid(const UiTextLayoutLimits &limits) const noexcept;
    };

    /** @brief One immutable line with positioned glyph and face-run ranges. */
    struct UiTextLayoutLine final {
        std::uint32_t firstGlyph{}; /**< First glyph in the result glyph table. */
        std::uint32_t glyphCount{}; /**< Number of positioned glyphs. */
        std::uint32_t firstRun{};   /**< First face run in the result run table. */
        std::uint32_t runCount{};   /**< Number of face runs for this line. */
        UiLogicalPoint origin;      /**< Top-left line origin in canvas logical units. */
        UiLogicalExtent extent;     /**< Line advance width and line height. */
        std::int32_t baseline{};    /**< Absolute logical baseline for this line. */
        std::int32_t advance{};     /**< Logical horizontal line advance before alignment. */
        bool hardBreak{};           /**< Whether the line ended at a mandatory source break. */
        bool ellipsis{};            /**< Whether an ellipsis was emitted for this line. */

        /** @brief Checks ranges against a complete immutable result. @return Whether the line is representable. */
        [[nodiscard]] bool IsValid(std::size_t glyphCountLimit, std::size_t runCountLimit) const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutLine &) const noexcept = default;
    };

    /** @brief One positioned backend-neutral glyph shared by layout and render extraction. */
    struct UiTextLayoutGlyph final {
        UiTextFaceId face;       /**< Stable shaped face identity for later atlas/resource resolution. */
        std::uint32_t glyph{};   /**< Backend-neutral glyph identity. */
        std::uint32_t cluster{}; /**< Source cluster index or NoUiTextLayoutCluster for ellipsis glyphs. */
        UiLogicalPoint origin;   /**< Positioned logical glyph origin. */
        UiLogicalPoint advance;  /**< Scaled logical advance used by measurement. */

        /** @brief Checks face, cluster sentinel, and non-negative advance evidence. @return Whether the glyph is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutGlyph &) const noexcept = default;
    };

    /** @brief One contiguous face range in the positioned result, suitable for render resource resolution. */
    struct UiTextLayoutRun final {
        UiTextFaceId face;          /**< Stable shaped face identity. */
        std::uint32_t firstGlyph{}; /**< First glyph in the result glyph table. */
        std::uint32_t glyphCount{}; /**< Number of glyphs in this run. */
        std::uint32_t line{};       /**< Owning line index. */
        bool ellipsis{};            /**< Whether this run contains pre-shaped ellipsis glyphs. */

        /** @brief Checks glyph and line ranges against a complete result. @return Whether the run is valid. */
        [[nodiscard]] bool IsValid(std::size_t glyphCountLimit, std::size_t lineCountLimit) const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutRun &) const noexcept = default;
    };

    /** @brief Measured and overflow evidence shared by layout, hit testing, and rendering. */
    struct UiTextLayoutOverflow final {
        UiLogicalExtent assigned; /**< Definite assigned content extent. */
        UiLogicalExtent desired;  /**< Clamped intrinsic desired extent. */
        UiLogicalExtent overflow; /**< Full unclipped logical text extent. */
        bool clipped{};           /**< True when the assigned box clips geometry. */
        bool truncated{};         /**< True when max-lines or ellipsis removed source content. */

        /** @brief Checks all extents and policy evidence. @return Whether overflow evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLayoutOverflow &) const noexcept = default;
    };

    /** @brief Exact immutable publication evidence for one text layout result. */
    struct UiTextLayoutResultDescriptor final {
        UiTextLayoutSource source;     /**< Exact source lineage consumed by the result. */
        UiTextLayoutRevision revision; /**< Monotonic layout publication revision. */
        UiTextLayoutOptions options;   /**< Immutable policy used to position the result. */

        /** @brief Checks source, revision, and options evidence. @return Whether descriptor is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    class UiTextLayoutEngine;

    /**
     * @brief Immutable leased text layout result shared by intrinsic consumers and render extraction.
     * @details The result owns positioned logical glyphs and line/run ranges. Renderer may resolve face identities to atlas
     * resources, but it cannot reshape, rewrap, change measurement, or mutate this result.
     */
    class UiTextLayoutResult final {
    public:
        /** @brief Releases this immutable result lease. */
        ~UiTextLayoutResult();
        /** @brief Retains one immutable result lease. @param other Result to retain. */
        UiTextLayoutResult(const UiTextLayoutResult &other) noexcept;
        /** @brief Replaces this lease by retained result. @param other Result to retain. @return This result. */
        UiTextLayoutResult &operator=(const UiTextLayoutResult &other) noexcept;
        /** @brief Transfers one immutable result lease. @param other Result to transfer. */
        UiTextLayoutResult(UiTextLayoutResult &&other) noexcept;
        /** @brief Replaces this lease by transfer. @param other Result to transfer. @return This result. */
        UiTextLayoutResult &operator=(UiTextLayoutResult &&other) noexcept;

        /** @brief Returns exact owner, source, and policy evidence. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiTextLayoutResultDescriptor &Descriptor() const noexcept;
        /** @brief Returns the layout-compatible intrinsic measurement. @return Borrowed immutable measurement. */
        [[nodiscard]] const UiLayoutMeasurement &Measurement() const noexcept;
        /** @brief Returns full measured and overflow evidence. @return Borrowed immutable overflow record. */
        [[nodiscard]] const UiTextLayoutOverflow &Overflow() const noexcept;
        /** @brief Returns lines in visual top-to-bottom order. @return Borrowed immutable line records. */
        [[nodiscard]] std::span<const UiTextLayoutLine> Lines() const noexcept;
        /** @brief Returns positioned glyphs in render order. @return Borrowed immutable glyph placements. */
        [[nodiscard]] std::span<const UiTextLayoutGlyph> Glyphs() const noexcept;
        /** @brief Returns face runs over the positioned glyph table. @return Borrowed immutable runs. */
        [[nodiscard]] std::span<const UiTextLayoutRun> Runs() const noexcept;
        /** @brief Checks that the result still owns a complete immutable publication. @return True for a live result. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        struct Storage;
        friend class UiTextLayoutEngine;
        explicit UiTextLayoutResult(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
    };

    /** @brief Fixed owner identities and preallocated result bounds for one text element. */
    struct UiTextLayoutEngineDescriptor final {
        RuntimeUiInstanceId instance;         /**< Exact runtime UI instance. */
        UiCanvasInstanceId canvas;            /**< Exact canvas incarnation. */
        UiDocumentId document;                /**< Stable source document. */
        UiElementHandle element;              /**< Exact retained text element. */
        UiTextLayoutLimits limits;            /**< Fixed source/output capacities. */
        std::uint32_t concurrentResults{};    /**< Number of immutable result slots. */
        UiTextLayoutRevision initialRevision; /**< First successful publication revision. */

        /** @brief Validates identities, capacities, and the initial revision. @return Whether the engine can be created. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Explicit admission state for one text-layout owner. */
    enum class UiTextLayoutEngineState : std::uint8_t {
        Active,
        Closed,
    };

    /**
     * @brief Owner-thread bounded text measurement and line-layout engine.
     * @details Creation reserves all scratch/output storage. Layout performs one bounded scan over shaped clusters and never
     *          performs I/O, waits, reshapes text, allocates fallback storage, or mutates the source/runtime tree.
     */
    class UiTextLayoutEngine final {
    public:
        /**
         * @brief Creates a text-layout owner with preallocated scratch and immutable result slots.
         * @param descriptor Exact element owner, capacities, and first publication revision.
         * @return Active engine or typed identity/capacity/allocation failure.
         */
        [[nodiscard]] static Result<UiTextLayoutEngine> Create(const UiTextLayoutEngineDescriptor &descriptor);
        /** @brief Closes admission while outstanding result leases remain valid. */
        ~UiTextLayoutEngine();
        /** @brief Transfers unique engine ownership. @param other Engine to transfer. */
        UiTextLayoutEngine(UiTextLayoutEngine &&other) noexcept;
        /** @brief Replaces this engine by transferred ownership. @param other Engine to transfer. @return This engine. */
        UiTextLayoutEngine &operator=(UiTextLayoutEngine &&other) noexcept;
        UiTextLayoutEngine(const UiTextLayoutEngine &) = delete;
        UiTextLayoutEngine &operator=(const UiTextLayoutEngine &) = delete;

        /**
         * @brief Measures, wraps, aligns, and publishes one immutable text result.
         * @param request Exact source, assigned content box, shaped input, and text policy.
         * @return Immutable result or typed validation, stale, capacity, slot, or lifecycle failure.
         * @pre Calls for one engine are serialized on its Runtime UI owner thread.
         */
        [[nodiscard]] Result<UiTextLayoutResult> Layout(const UiTextLayoutRequest &request);
        /** @brief Stops new layout calls without invalidating existing result leases. */
        void Close() noexcept;
        /** @brief Idempotently closes admission and releases mutable scratch storage. */
        void Shutdown() noexcept;
        /** @brief Reports whether all immutable result slots are unleased. @return True when the owner is drained. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns the explicit admission state. @return Active or Closed. */
        [[nodiscard]] UiTextLayoutEngineState State() const noexcept;

    private:
        struct Storage;
        explicit UiTextLayoutEngine(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
