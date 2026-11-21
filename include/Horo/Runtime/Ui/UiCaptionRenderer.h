#pragma once

/** @file UiCaptionRenderer.h
 * @brief Bounded caption paint extraction through the canonical Runtime UI renderer contract.
 */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Ui/UiCaptions.h"
#include "Horo/Runtime/Ui/UiRenderSnapshot.h"

namespace Horo::Runtime::Ui {
    /** @brief Call-duration prepared text from the existing localization/shaping/layout owner.
     * @details Glyph origins are local to measured.extent, including readable text padding. Every glyph quad fits
     * that extent; no scaling, truncation, glyph lookup, fallback text or native resource discovery occurs here.
     * The text owner resolves the pinned catalog against one locale snapshot, includes the requested speaker/direction
     * labels, and prepares wrapped glyphs using exactly appearance.fontScale before this render call. Catalog identity
     * and the complete appearance are checked against the timeline/frame; this value is not a localization resolver.
     */
    struct CaptionPaintCue final {
        CaptionMeasuredCue measured;
        UiElementHandle element; /**< Existing retained caption text element in the supplied canonical tree. */
        std::shared_ptr<const std::vector<LocalizedMessageRef>> catalog;
        CaptionAppearance appearance;
        Sha256Digest
            localization; /**< Host commitment to the exact locale selection and immutable localization snapshot used for shaping. */
        std::span<const UiPositionedGlyph> glyphs;
        std::span<const UiTextRun> runs; /**< Local font-resource/glyph indices; colors are replaced by the frame appearance. */
        std::span<const UiRenderResourceReference> resources; /**< Exact FontFace generations, owned through normal renderer residency. */
    };

    /** @brief Placement diagnostics and the canonical immutable paint publication from the same frame. */
    struct CaptionRenderFrame final {
        CaptionLayout layout;
        UiRenderSnapshot snapshot;
        Sha256Digest localization;
    };

    /** @brief Owner-thread preallocated caption presenter; owns paint scratch, never another retained tree or text authority.
     * @details Setup reserves all scratch and ordinary render snapshot slots. Render copies bounded text geometry and
     * stable resources into a canonical UiRenderSnapshot without allocating on success. Outstanding leases remain readable
     * after presenter destruction; ordinary renderer resource residency and native retirement rules still apply.
     */
    class CaptionRenderer final {
    public:
        /** @brief Reserves a bounded canonical render extractor and matching scratch storage before frame work.
         * @param descriptor Exact view, table capacities and simultaneous snapshot limit supplied by the host.
         * @return Presenter or original render-extractor/caption capacity error.
         */
        [[nodiscard]] static Result<CaptionRenderer> Create(const UiRenderExtractorDescriptor &descriptor);
        CaptionRenderer(CaptionRenderer &&) noexcept = default;
        CaptionRenderer &operator=(CaptionRenderer &&) noexcept = default;
        CaptionRenderer(const CaptionRenderer &) = delete;
        CaptionRenderer &operator=(const CaptionRenderer &) = delete;
        /** @brief Arranges eligible cues and publishes solid backgrounds and shaped text in canonical paint order.
         * @param tree Actual retained tree owning each caption element; no elements are created or mutated.
         * @param descriptor Exact source/view and increasing render publication revision.
         * @param viewport Positive logical reference viewport used for safe-area arrangement.
         * @param timeline Semantic timeline already drained/advanced at this UI frame boundary.
         * @param appearance One validated frame capture. Colors are encoded sRGB RGBA, converted to linear paint here.
         * @param localization Nonzero host commitment to this frame's locale selection and immutable localization snapshot.
         * @param prepared Bounded prepared text for every currently visible eligible cue; duplicate/foreign/stale inputs fail.
         * @param frame Exact current canonical animation publication when composed by an animation owner; null for non-animated extraction.
         * @return Complete immutable paint frame or typed input/capacity/render failure. Failure publishes no snapshot.
         * @details Future cues never paint; Sound/Music obey showSoundDescriptions; disabling paints an empty snapshot.
         * Oversized measured cues remain in layout.unplaced, never shrink below their prepared typography. The host appends
         * this presentation to its normal composed UI path; successful extraction alone does not prove native display.
         * An animated host must supply its real frame lease; canonical extraction validates and retains it through queued readers.
         */
        [[nodiscard]] Result<CaptionRenderFrame> Render(const UiElementTree &tree, const UiRenderSnapshotDescriptor &descriptor,
                                                        UiLogicalRect viewport, const CaptionTimeline &timeline,
                                                        const CaptionAppearance &appearance, const Sha256Digest &localization,
                                                        std::span<const CaptionPaintCue> prepared,
                                                        const UiAnimationFrameLease *frame = nullptr);

    private:
        /** @brief Takes the canonical extractor and reserves matching bounded scratch; setup may allocate. */
        CaptionRenderer(UiRenderExtractor extractor, UiRenderSnapshotLimits limits);
        /** @brief Copies one placed cue into bounded scratch after validating local geometry and indices.
         * @param cue Prepared text at this frame's appearance. @param placement Safe-area position.
         * @param text Linear text color. @param background Linear background with final opacity.
         * @return Success or typed malformed/capacity error, with no published snapshot on failure.
         */
        [[nodiscard]] Result<void> AppendCue(const CaptionPaintCue &cue, const CaptionPlacement &placement, UiLinearColor text,
                                             UiLinearColor background);
        UiRenderExtractor extractor_;
        UiRenderSnapshotLimits limits_;
        std::vector<UiDrawCommand> commands_;
        std::vector<UiTextRun> runs_;
        std::vector<UiPositionedGlyph> glyphs_;
        std::vector<UiRenderResourceReference> resources_;
    };
}  // namespace Horo::Runtime::Ui
