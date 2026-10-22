#pragma once

/** @file UiPresentationProfiles.h
 * @brief Finite document-owned layout variants selected from explicit attachment evidence.
 */
#include "Horo/Runtime/Ui/UiLayout.h"

#include <vector>

namespace Horo::Runtime::Ui {
    class UiReloadGeneration;

    inline constexpr std::uint32_t MaximumUiPresentationProfiles = 32;
    inline constexpr std::uint32_t MaximumUiPresentationOverrides = 4096;
    inline constexpr std::size_t MaximumUiPresentationTableBytes = 16U * 1024U * 1024U;

    struct UiPresentationProfileIdentityTag;
    struct UiPresentationProfileRevisionTag;
    struct UiPresentationEvidenceRevisionTag;
    struct UiPresentationAttachmentHandleTag;
    /** @brief Stable profile identity local to one authored document. */
    using UiPresentationProfileId = UiStableId<UiPresentationProfileIdentityTag>;
    /** @brief Exact immutable document profile table revision. */
    using UiPresentationProfileRevision = UiRevision<UiPresentationProfileRevisionTag>;
    /** @brief Exact attachment-owner evidence revision. */
    using UiPresentationEvidenceRevision = UiRevision<UiPresentationEvidenceRevisionTag>;
    /** @brief Owner-issued derived geometry attachment incarnation; never a semantic UI owner. */
    using UiPresentationAttachmentId = UiRuntimeHandle<UiPresentationAttachmentHandleTag>;

    /** @brief Explicit presentation classification; Unknown never infers a platform. */
    enum class UiPresentationFormFactor : std::uint8_t {
        Unknown,
        Desktop,
        Handheld,
        Television
    };
    /** @brief Independent local viewport role. */
    enum class UiPresentationViewRole : std::uint8_t {
        Single,
        Split
    };
    /** @brief Admitted available interaction capabilities, independent of last-used modality. */
    enum class UiPresentationCapabilities : std::uint8_t {
        None = 0,
        Pointer = 1U << 0U,
        Keyboard = 1U << 1U,
        Gamepad = 1U << 2U,
        Touch = 1U << 3U,
    };

    /** @brief Optional positive half-open extent bounds in canonical 1/64-DIP units. */
    struct UiPresentationExtentRange final {
        std::optional<std::int32_t> minimum; /**< Inclusive lower bound; absent imposes no lower condition. */
        std::optional<std::int32_t> maximum; /**< Exclusive upper bound; absent is unbounded. */
        /** @brief Checks positive nonempty bounds. @return Whether the interval is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiPresentationExtentRange &) const noexcept = default;
    };

    /** @brief Positive reduced exact width-to-height ratio. */
    struct UiPresentationAspectRatio final {
        std::uint32_t numerator{};
        std::uint32_t denominator{};
        /** @brief Checks positive reduced representation. @return Whether this ratio is canonical. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiPresentationAspectRatio &) const noexcept = default;
    };

    /** @brief Optional half-open exact aspect interval. */
    struct UiPresentationAspectRange final {
        std::optional<UiPresentationAspectRatio> minimum;
        std::optional<UiPresentationAspectRatio> maximum;
        /** @brief Checks reduced nonempty bounds with exact integer products. @return Whether valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiPresentationAspectRange &) const noexcept = default;
    };

    /** @brief Conjunction of independent optional conditions for one profile. */
    struct UiPresentationProfileSelector final {
        UiPresentationExtentRange width;
        UiPresentationExtentRange height;
        UiPresentationAspectRange aspect;
        std::optional<UiPresentationFormFactor> formFactor;
        std::optional<UiPresentationViewRole> viewRole;
        UiPresentationCapabilities required{UiPresentationCapabilities::None};
        /** @brief Checks ranges and all closed enum values. @return Whether this selector is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiPresentationProfileSelector &) const noexcept = default;
    };

    /** @brief Complete layout-only replacement; semantic state and identity cannot be changed. */
    struct UiPresentationLayoutOverride final {
        UiElementId element;
        UiLayoutStyle layout;
        [[nodiscard]] auto operator<=>(const UiPresentationLayoutOverride &) const noexcept = default;
    };

    /** @brief One authored alternative; priorities and profile IDs must be unique across the document. */
    struct UiPresentationProfile final {
        UiPresentationProfileId id;
        std::uint32_t priority{};
        UiPresentationProfileSelector selector;
        std::vector<UiPresentationLayoutOverride> overrides;
        [[nodiscard]] auto operator<=>(const UiPresentationProfile &) const noexcept = default;
    };

    /** @brief Durable document-owned alternatives for exactly one authored canvas. */
    struct UiDocumentPresentationProfiles final {
        UiCanvasId canvas;
        UiPresentationProfileRevision revision;
        std::vector<UiPresentationProfile> rows;
        [[nodiscard]] auto operator<=>(const UiDocumentPresentationProfiles &) const noexcept = default;
    };

    /** @brief Validates finite inert rows independently of runtime owner creation.
     * @param rows Candidate durable rows. @return Typed malformed, duplicate, unsupported or capacity failure.
     */
    [[nodiscard]] Result<void> ValidateUiPresentationProfiles(std::span<const UiPresentationProfile> rows);

    /** @brief Frozen actual attachment facts; no input or publication authority is conveyed. */
    struct UiPresentationProfileEvidence final {
        UiCanvasId canvas; /**< Exact authored canvas; membership cannot cross canvases. */
        RuntimeUiInstanceId instance;
        UiCanvasInstanceId runtimeCanvas;
        UiRuntimeTreeRevision tree; /**< Exact current expanded tree, refreshed with ordinary structural updates. */
        UiPresentationAttachmentId attachment;
        UiPresentationEvidenceRevision revision;
        UiCanvasLogicalExtent content; /**< Already resolved safe logical content; no second DPI/scale conversion. */
        UiPresentationFormFactor formFactor{UiPresentationFormFactor::Unknown};
        UiPresentationViewRole viewRole{UiPresentationViewRole::Single};
        UiPresentationCapabilities available{UiPresentationCapabilities::None};
        /** @brief Checks exact identity, positive extent and all closed enum values. @return Whether facts are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const UiPresentationProfileEvidence &) const noexcept = default;
    };

    /** @brief Immutable copied selection evidence for one exact attachment, table and document generation. */
    struct UiPresentationProfileSelection final {
        UiDocumentId document;
        UiDocumentRevision documentRevision;
        UiCanvasId canvas;
        UiPresentationProfileRevision tableRevision;
        UiPresentationProfileEvidence evidence;
        std::optional<UiPresentationProfileId> profile; /**< Absent selects the mandatory neutral base. */
        std::optional<std::uint32_t> priority;          /**< Present exactly when a row was selected. */
        [[nodiscard]] auto operator<=>(const UiPresentationProfileSelection &) const noexcept = default;
    };

    /**
     * @brief Immutable validated finite alternatives owned by the exact durable document generation.
     * @details Create is load-time and may allocate. Selection and resolution only scan bounded immutable data.
     * Selected complete layout is the attachment-local base before existing computed-style ProjectLayout bindings;
     * explicitly bound computed dimensions, sealing and accessibility precedence remain final. Profiles never mutate
     * semantic focus, controls, bindings or routes. Caller publication must correlate selection with actual layout/clip/focus.
     */
    class UiPresentationProfileTable final {
    public:
        /** @brief Validates runtime alternatives against an already admitted cooked generation and its actual canvas tree.
         * @param generation Actual admitted generation; raw durable row validation stays separate from runtime admission.
         * @param canvas Exact authored canvas. @param revision Nonzero immutable table revision.
         * @param rows At most 32 rows and 4096 total overrides; byte/count ceilings are checked before copying.
         * @return Canonical table or typed invalid, capacity or foreign-target failure.
         */
        [[nodiscard]] static Result<UiPresentationProfileTable> Create(const UiReloadGeneration &generation, UiCanvasId canvas,
                                                                       UiPresentationProfileRevision revision,
                                                                       std::span<const UiPresentationProfile> rows);
        /** @brief Borrows canonical rows in ascending unique priority. @return Rows valid for this table lifetime. */
        [[nodiscard]] std::span<const UiPresentationProfile> Rows() const noexcept;
        /** @brief Selects the first complete match, or neutral base, without allocation or previous-selection bias.
         * @param tree Actual current tree from the admitted owner. @param evidence Actual resolved attachment facts.
         * @return Exact selection or typed malformed evidence failure; malformed evidence never silently chooses base.
         */
        [[nodiscard]] Result<UiPresentationProfileSelection> Select(const UiElementTree &tree,
                                                                    const UiPresentationProfileEvidence &evidence) const;
        /** @brief Resolves a complete selected layout before existing computed property bindings are projected.
         * @param tree Actual current owner tree. @param selection Exact selection issued for this table and evidence. @param element Stable
         * source element.
         * @param base Ordinary base layout for an untouched element or neutral selection.
         * @return Complete selected layout or base; stale/forged selection and invalid base fail explicitly.
         */
        [[nodiscard]] Result<UiLayoutStyle> ResolveLayout(const UiElementTree &tree, const UiPresentationProfileSelection &selection,
                                                          UiElementId element, const UiLayoutStyle &base) const;

        /** @brief Validates the full current element set before applying selected bases without allocation.
         * @param tree Actual admitted current tree. @param selection Exact current selection.
         * @param descriptors One valid base descriptor per tree element, sorted by exact element handle.
         * @return Success or typed failure leaving all descriptors unchanged. Existing ProjectLayout runs afterwards.
         */
        [[nodiscard]] Result<void> ApplyLayouts(const UiElementTree &tree, const UiPresentationProfileSelection &selection,
                                                std::span<UiLayoutElementDescriptor> descriptors) const;

    private:
        /** @brief Copies admitted owner identity and adopts validated canonical immutable rows. */
        UiPresentationProfileTable(const UiElementTree &tree, UiCanvasId canvas, UiPresentationProfileRevision revision,
                                   std::vector<UiPresentationProfile> rows) noexcept;
        /** @brief Checks actual current owner identity while permitting ordinary same-issuer tree revision advances. */
        [[nodiscard]] bool MatchesTree(const UiElementTree &tree, const UiPresentationProfileEvidence &evidence) const noexcept;
        UiDocumentId document_;
        UiDocumentRevision documentRevision_;
        UiCanvasId canvas_;
        RuntimeUiInstanceId instance_;
        UiCanvasInstanceId runtimeCanvas_;
        UiElementHandle root_;
        std::shared_ptr<const void> issuer_;
        UiPresentationProfileRevision revision_;
        std::vector<UiPresentationProfile> rows_;
    };
}  // namespace Horo::Runtime::Ui
