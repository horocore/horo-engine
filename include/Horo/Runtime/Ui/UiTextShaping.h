#pragma once

/**
 * @file UiTextShaping.h
 * @brief Backend-neutral Unicode shaping, fallback, measurement, and glyph-run contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Ui/UiIdentity.h"
#include "Horo/Runtime/Ui/UiLayout.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime::Ui {
    inline constexpr std::size_t MaximumUiFontFaceBytes = 64U * 1024U * 1024U;
    inline constexpr std::uint32_t MaximumUiFallbackFaces = 32;
    inline constexpr std::uint32_t MaximumUiTextLanguageBytes = 32;
    inline constexpr std::uint32_t MaximumUiTextFeatures = 32;
    inline constexpr std::uint32_t MaximumUiTextInputBytes = 1U * 1024U * 1024U;
    inline constexpr std::uint32_t MaximumUiTextGlyphs = 65536;
    inline constexpr std::uint32_t MaximumUiTextClusters = 65536;
    inline constexpr std::uint32_t MaximumUiTextRuns = 4096;
    inline constexpr std::uint32_t MaximumUiTextShapesInFlight = 8;
    inline constexpr std::uint32_t NoUiTextIndex = std::numeric_limits<std::uint32_t>::max();
    inline constexpr std::uint32_t NoUiTextFeatureEnd = std::numeric_limits<std::uint32_t>::max();

    /** @brief Identifies one immutable cooked font face payload and collection face. */
    struct UiFontFaceDescriptor final {
        UiFontFaceId id;                 /**< Stable authored face identity. */
        std::uint32_t collectionIndex{}; /**< Zero-based face index in the immutable font container. */
        /** @brief Checks stable identity representation. @return True when the face identity is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiFontFaceDescriptor &) const noexcept = default;
    };

    /**
     * @brief Owns one immutable backend-neutral font face byte payload.
     * @details The payload is copied at creation. Runtime callers provide cooked bytes; this contract performs no file or
     * platform-font discovery and exposes no shaping-library handles.
     */
    class UiFontFace final {
    public:
        /**
         * @brief Validates and copies one cooked font face.
         * @param descriptor Stable face identity and collection index.
         * @param bytes Complete bounded OpenType-compatible payload.
         * @return Immutable face or UiErrors::TextFontInvalid.
         */
        [[nodiscard]] static Result<UiFontFace> Create(const UiFontFaceDescriptor &descriptor, std::span<const std::uint8_t> bytes);
        /** @brief Releases the immutable face payload. */
        ~UiFontFace();
        /** @brief Retains the immutable face payload. @param other Face to retain. */
        UiFontFace(const UiFontFace &other) noexcept;
        /** @brief Replaces this face by retained immutable payload. @param other Face to retain. @return This face. */
        UiFontFace &operator=(const UiFontFace &other) noexcept;
        /** @brief Transfers one immutable face payload. @param other Face to move. */
        UiFontFace(UiFontFace &&other) noexcept;
        /** @brief Replaces this face by transferred payload. @param other Face to move. @return This face. */
        UiFontFace &operator=(UiFontFace &&other) noexcept;

        /** @brief Returns the stable authored face identity. @return Stable face identity, or invalid when this is empty. */
        [[nodiscard]] UiFontFaceId Id() const noexcept;
        /** @brief Returns the selected collection face index. @return Zero-based collection index. */
        [[nodiscard]] std::uint32_t CollectionIndex() const noexcept;
        /** @brief Returns the validated design-unit scale. @return Units per em, or zero when this is empty. */
        [[nodiscard]] std::uint32_t UnitsPerEm() const noexcept;
        /** @brief Checks that immutable face storage is present. @return True when this face can be used for shaping. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        struct Storage;
        explicit UiFontFace(std::shared_ptr<const Storage> storage) noexcept;
        std::shared_ptr<const Storage> storage_;
        friend class UiTextShaper;
    };

    /** @brief Explicit behavior when an entire grapheme cluster has no covering fallback face. */
    enum class UiMissingGlyphPolicy : std::uint8_t {
        Replacement,
        OmitWithAdvance,
        FailStrict,
        Count
    };

    /**
     * @brief Validated ordered immutable fallback chain consumed by shaping.
     * @details Faces are tried in order for a complete grapheme cluster. The chain never performs per-codepoint fallback or
     * hidden platform-font discovery.
     */
    class UiFontFallbackChain final {
    public:
        /**
         * @brief Copies and validates an ordered fallback chain.
         * @param faces Ordered primary-to-terminal immutable faces.
         * @param missingGlyphPolicy Policy for clusters absent from every face.
         * @return Validated chain or UiErrors::TextFallbackInvalid.
         */
        [[nodiscard]] static Result<UiFontFallbackChain> Create(std::span<const UiFontFace> faces, UiMissingGlyphPolicy missingGlyphPolicy);

        /** @brief Returns the ordered immutable faces. @return Primary-to-terminal face sequence. */
        [[nodiscard]] std::span<const UiFontFace> Faces() const noexcept;
        /** @brief Returns the terminal replacement face, or nullptr for an invalid chain. @return Borrowed terminal face. */
        [[nodiscard]] const UiFontFace *Terminal() const noexcept;
        /** @brief Returns the explicit missing-glyph behavior. @return Missing-glyph policy. */
        [[nodiscard]] UiMissingGlyphPolicy MissingPolicy() const noexcept;
        /** @brief Checks count, face validity, and unique stable identity. @return True when the chain is usable. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        std::vector<UiFontFace> faces_;
        UiMissingGlyphPolicy missingGlyphPolicy_{UiMissingGlyphPolicy::Replacement};
    };

    /** @brief Four-letter ISO 15924-like script evidence, with an Auto sentinel. */
    class UiTextScript final {
    public:
        /** @brief Returns script auto-detection evidence. @return Auto script. */
        [[nodiscard]] static UiTextScript Auto() noexcept;
        /**
         * @brief Validates and normalizes a four-letter script tag.
         * @param value ASCII script tag such as Latn or Arab.
         * @return Normalized script or UiErrors::TextInputInvalid.
         */
        [[nodiscard]] static Result<UiTextScript> Create(std::string_view value);
        /** @brief Checks Auto or normalized four-letter ASCII script representation. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Reports whether this value requests script detection. @return True for Auto. */
        [[nodiscard]] bool IsAuto() const noexcept;
        /** @brief Returns the normalized four-letter tag. @return Empty view for Auto. */
        [[nodiscard]] std::string_view Value() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextScript &) const noexcept = default;

    private:
        std::array<char, 4> value_{};
    };

    /** @brief Bounded normalized BCP-47-style language evidence, with an Auto sentinel. */
    class UiTextLanguage final {
    public:
        /** @brief Returns language auto-detection evidence. @return Auto language. */
        [[nodiscard]] static UiTextLanguage Auto() noexcept;
        /**
         * @brief Validates and lowercases a bounded language tag.
         * @param value ASCII language tag such as en-US.
         * @return Normalized language or UiErrors::TextInputInvalid.
         */
        [[nodiscard]] static Result<UiTextLanguage> Create(std::string_view value);
        /** @brief Checks Auto or bounded ASCII language representation. @return True when valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Reports whether this value requests language detection. @return True for Auto. */
        [[nodiscard]] bool IsAuto() const noexcept;
        /** @brief Returns the normalized language tag. @return Empty view for Auto. */
        [[nodiscard]] std::string_view View() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextLanguage &) const noexcept = default;

    private:
        std::array<char, MaximumUiTextLanguageBytes> value_{};
        std::uint8_t size_{};
    };

    /** @brief Explicit shaping direction; Auto resolves from the selected script per run. */
    enum class UiTextDirection : std::uint8_t {
        Auto,
        LeftToRight,
        RightToLeft,
        TopToBottom,
        BottomToTop,
        Count
    };

    /** @brief One bounded OpenType feature request applied over source byte offsets. */
    struct UiTextFeature final {
        std::array<char, 4> tag{};             /**< Four-byte OpenType feature tag. */
        std::uint32_t value{1};                /**< Feature value, conventionally zero or one. */
        std::uint32_t start{};                 /**< Inclusive UTF-8 source byte offset. */
        std::uint32_t end{NoUiTextFeatureEnd}; /**< Exclusive UTF-8 source byte offset, or global sentinel. */

        /**
         * @brief Validates and creates one feature request.
         * @param tag Four printable ASCII letters or digits.
         * @param value OpenType feature value.
         * @param start Inclusive source byte offset.
         * @param end Exclusive source byte offset, or NoUiTextFeatureEnd.
         * @return Feature or UiErrors::TextFeatureInvalid.
         */
        [[nodiscard]] static Result<UiTextFeature> Create(std::string_view tag, std::uint32_t value = 1, std::uint32_t start = 0,
                                                          std::uint32_t end = NoUiTextFeatureEnd);
        /** @brief Checks tag bytes and range ordering. @return True when the feature is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the four-byte feature tag. @return Borrowed tag view. */
        [[nodiscard]] std::string_view Tag() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextFeature &) const noexcept = default;
    };

    /** @brief Positive font size in the shared 1/64-DIP logical unit. */
    struct UiTextFontSize final {
        std::int32_t value{1024}; /**< 1/64-DIP em size; 1024 is 16 DIP. */

        /** @brief Validates a logical font size. @param value 1/64-DIP em size. @return Size or UiErrors::TextInputInvalid. */
        [[nodiscard]] static Result<UiTextFontSize> Create(std::int32_t value);
        /** @brief Checks positivity and bounded representability. @return True when usable by HarfBuzz scaling. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiTextFontSize &) const noexcept = default;
    };

    /** @brief Bounded storage and concurrency limits reserved by one shaper. */
    struct UiTextShaperLimits final {
        std::uint32_t maxInputBytes{MaximumUiTextInputBytes};
        std::uint32_t maxFeatures{MaximumUiTextFeatures};
        std::uint32_t maxGlyphs{MaximumUiTextGlyphs};
        std::uint32_t maxClusters{MaximumUiTextClusters};
        std::uint32_t maxRuns{MaximumUiTextRuns};
        std::uint32_t concurrentShapes{MaximumUiTextShapesInFlight};

        /** @brief Checks every limit is positive and within the global contract. @return True when preallocation is bounded. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Non-owning input text and shaping evidence captured for one request. */
    struct UiTextShapingRequest final {
        std::string_view text;                            /**< Complete UTF-8 scalar sequence; copied into the result. */
        UiTextContentRevision content;                    /**< Source-content revision. */
        UiTextScript script;                              /**< Requested or Auto script. */
        UiTextLanguage language;                          /**< Requested or Auto language. */
        UiTextDirection direction{UiTextDirection::Auto}; /**< Requested direction. */
        UiTextFontSize fontSize;                          /**< Logical em size. */
        std::span<const UiTextFeature> features;          /**< Bounded feature requests. */

        /**
         * @brief Validates request evidence against reserved limits.
         * @param limits Limits reserved by the owning shaper.
         * @return True when text, revisions, tags, direction, size, and features are valid.
         */
        [[nodiscard]] bool IsValid(const UiTextShaperLimits &limits) const noexcept;
    };

    /** @brief One positioned glyph with stable face identity and UTF-8 source cluster offset. */
    struct UiTextGlyph final {
        UiFontFaceId face;       /**< Immutable face identity selected by fallback. */
        std::uint32_t glyph{};   /**< Backend-neutral glyph ID. */
        std::uint32_t cluster{}; /**< Inclusive UTF-8 source byte offset. */
        UiLogicalPoint offset;   /**< 1/64-DIP glyph offset from the run pen. */
        UiLogicalPoint advance;  /**< 1/64-DIP logical glyph advance. */

        /** @brief Checks stable face and bounded cluster representation. @return True when glyph evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Contiguous glyph and cluster range selected from one immutable face. */
    struct UiTextGlyphRun final {
        UiFontFaceId face;                                       /**< Immutable face identity selected for this run. */
        UiTextScript script;                                     /**< Resolved script evidence. */
        UiTextDirection direction{UiTextDirection::LeftToRight}; /**< Resolved shaping direction. */
        std::uint32_t firstGlyph{};                              /**< First glyph index, or NoUiTextIndex when glyphCount is zero. */
        std::uint32_t glyphCount{};                              /**< Number of entries in UiTextShape::Glyphs(). */
        std::uint32_t firstCluster{};                            /**< First cluster index. */
        std::uint32_t clusterCount{};                            /**< Number of entries in UiTextShape::Clusters(). */
        std::uint32_t byteStart{};                               /**< Inclusive UTF-8 source byte range. */
        std::uint32_t byteEnd{};                                 /**< Exclusive UTF-8 source byte range. */
        UiLogicalPoint advance;                                  /**< Sum of logical glyph advances. */

        /** @brief Checks range, direction, and logical advance representation. @return True when run evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Source cluster-to-glyph mapping used by measurement and editing. */
    struct UiTextCluster final {
        std::uint32_t byteStart{};               /**< Inclusive UTF-8 source byte offset. */
        std::uint32_t byteEnd{};                 /**< Exclusive UTF-8 source byte offset. */
        std::uint32_t firstGlyph{NoUiTextIndex}; /**< First mapped glyph, or no glyph for omitted coverage. */
        std::uint32_t glyphCount{};              /**< Number of mapped glyphs. */
        UiLogicalPoint advance;                  /**< Advance attributed to this shaping cluster. */
        bool missing{};                          /**< True when explicit missing-glyph policy was used. */

        /** @brief Checks byte ordering and optional glyph range representation. @return True when cluster evidence is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Stable logical measurement and font metrics for one shaped text result. */
    struct UiTextMetrics final {
        UiLogicalPoint advance; /**< Total logical advance in 1/64-DIP units. */
        std::int32_t ascent{};  /**< Non-negative ascender in 1/64-DIP units. */
        std::int32_t descent{}; /**< Non-negative descent magnitude in 1/64-DIP units. */
        std::int32_t lineGap{}; /**< Non-negative line gap in 1/64-DIP units. */
        UiLogicalExtent bounds; /**< Conservative non-negative logical ink/layout bounds. */

        /** @brief Checks non-negative metrics and extents. @return True when measurement is representable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact owner and source lineage captured by one immutable shape result. */
    struct UiTextShapeDescriptor final {
        UiOwnershipGeneration ownership;                           /**< Runtime UI owner generation. */
        UiTextContentRevision content;                             /**< Source-content revision. */
        UiTextFontRevision font;                                   /**< Immutable font-registry revision. */
        UiTextShapeRevision shape;                                 /**< Shaped-result publication revision. */
        UiTextScript requestedScript;                              /**< Request script evidence. */
        UiTextScript resolvedScript;                               /**< Resolved script for the result, or Auto for mixed runs. */
        UiTextLanguage language;                                   /**< Request language evidence. */
        UiTextDirection requestedDirection{UiTextDirection::Auto}; /**< Request direction. */
        UiTextDirection resolvedDirection{UiTextDirection::Auto};  /**< Resolved direction, or Auto for mixed runs. */
        UiTextFontSize fontSize;                                   /**< Logical em size used to scale metrics. */

        /** @brief Checks all source lineage and shaping evidence. @return True when descriptor is complete. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable leased result of one Unicode shaping transaction. */
    class UiTextShape final {
    public:
        /** @brief Releases this immutable shape lease. */
        ~UiTextShape();
        /** @brief Retains the exact immutable shape. @param other Shape to retain. */
        UiTextShape(const UiTextShape &other) noexcept;
        /** @brief Replaces this lease by retained shape. @param other Shape to retain. @return This shape. */
        UiTextShape &operator=(const UiTextShape &other) noexcept;
        /** @brief Transfers one immutable shape lease. @param other Shape to move. */
        UiTextShape(UiTextShape &&other) noexcept;
        /** @brief Replaces this lease by transferred shape. @param other Shape to move. @return This shape. */
        UiTextShape &operator=(UiTextShape &&other) noexcept;

        /** @brief Returns exact owner and source lineage. @return Borrowed immutable descriptor. */
        [[nodiscard]] const UiTextShapeDescriptor &Descriptor() const noexcept;
        /** @brief Returns copied source UTF-8 text. @return Borrowed immutable text bytes. */
        [[nodiscard]] std::string_view Text() const noexcept;
        /** @brief Returns copied feature requests. @return Borrowed immutable features. */
        [[nodiscard]] std::span<const UiTextFeature> Features() const noexcept;
        /** @brief Returns stable logical measurement. @return Borrowed immutable metrics. */
        [[nodiscard]] const UiTextMetrics &Metrics() const noexcept;
        /** @brief Returns contiguous face-selected glyph runs. @return Borrowed immutable runs. */
        [[nodiscard]] std::span<const UiTextGlyphRun> Runs() const noexcept;
        /** @brief Returns positioned glyphs referenced by runs. @return Borrowed immutable glyphs. */
        [[nodiscard]] std::span<const UiTextGlyph> Glyphs() const noexcept;
        /** @brief Returns source clusters referenced by runs. @return Borrowed immutable cluster mapping. */
        [[nodiscard]] std::span<const UiTextCluster> Clusters() const noexcept;
        /** @brief Checks that the lease still names a complete immutable result. @return True when this shape is non-empty and valid. */
        [[nodiscard]] bool IsValid() const noexcept;

    private:
        struct Storage;
        explicit UiTextShape(std::shared_ptr<const Storage> storage) noexcept;
        void Retain() const noexcept;
        void Release() noexcept;
        std::shared_ptr<const Storage> storage_;
        friend class UiTextShaper;
    };

    /** @brief Admission lifecycle of one owner-thread text shaper. */
    enum class UiTextShaperState : std::uint8_t {
        Active,
        Closed
    };

    /** @brief Exact immutable inputs and limits used to construct one text shaper. */
    struct UiTextShaperDescriptor final {
        UiOwnershipGeneration ownership; /**< Runtime UI owner generation. */
        UiTextFontRevision font;         /**< Immutable font-registry revision. */
        UiFontFallbackChain fonts;       /**< Ordered immutable fallback chain. */
        UiTextShaperLimits limits;       /**< Preallocated result and input bounds. */

        /** @brief Checks owner, font generation, fallback chain, and limits. @return True when construction is allowed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Owner-thread bounded Unicode shaper and immutable result publisher.
     * @details HarfBuzz and UTF-8 implementation details remain private. Calls are serialized by the Runtime UI owner;
     * returned results are immutable and may outlive this shaper until their leases retire.
     */
    class UiTextShaper final {
    public:
        /**
         * @brief Allocates bounded slots and validates the immutable fallback chain.
         * @param descriptor Owner generation, font revision, chain, and limits.
         * @return Active shaper or typed font, capacity, or identity failure.
         */
        [[nodiscard]] static Result<UiTextShaper> Create(const UiTextShaperDescriptor &descriptor);
        /** @brief Closes admission and releases backend shaping state after outstanding results retire. */
        ~UiTextShaper();
        /** @brief Transfers shaper ownership. @param other Shaper to move. */
        UiTextShaper(UiTextShaper &&other) noexcept;
        /** @brief Replaces this shaper by transferred ownership. @param other Shaper to move. @return This shaper. */
        UiTextShaper &operator=(UiTextShaper &&other) noexcept;
        UiTextShaper(const UiTextShaper &) = delete;
        UiTextShaper &operator=(const UiTextShaper &) = delete;

        /**
         * @brief Validates, shapes, measures, and publishes one complete UTF-8 request.
         * @param request Non-owning text and shaping evidence; text is copied into the result.
         * @return Immutable result lease or typed validation, missing-coverage, capacity, or lifecycle failure.
         * @pre Calls for one shaper are serialized on its Runtime UI owner thread.
         */
        [[nodiscard]] Result<UiTextShape> Shape(const UiTextShapingRequest &request);
        /** @brief Stops new shaping while existing immutable result leases remain valid. */
        void Close() noexcept;
        /** @brief Idempotently closes admission and releases transient shaping state. */
        void Shutdown() noexcept;
        /** @brief Reports whether all immutable result slots are unleased. @return True when retirement may finish. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns current shaping admission lifecycle. @return Active or Closed. */
        [[nodiscard]] UiTextShaperState State() const noexcept;

    private:
        struct Storage;
        explicit UiTextShaper(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
