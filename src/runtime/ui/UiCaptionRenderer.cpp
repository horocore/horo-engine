#include "Horo/Runtime/Ui/UiCaptionRenderer.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Decodes an already validated encoded sRGB component for canonical linear UI paint. */
        float DecodeSrgb(const float value) noexcept {
            return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
        }

        /** @brief Preserves alpha while decoding a validated appearance color. */
        UiLinearColor LinearColor(const std::array<float, 4> &color) noexcept {
            return {DecodeSrgb(color[0]), DecodeSrgb(color[1]), DecodeSrgb(color[2]), color[3]};
        }

        /** @brief Applies only semantic time and caption-owned display preferences, never audio/device/backend state. */
        bool Visible(const CaptionActiveCue &active, const CaptionTimeline &timeline, const CaptionAppearance &appearance) noexcept {
            return appearance.enabled && active.cue.startTick <= timeline.Tick() &&
                   timeline.Tick() - active.cue.startTick < active.cue.durationTicks &&
                   (active.cue.kind == CaptionCueKind::Dialogue || appearance.showSoundDescriptions);
        }

        /** @brief Rejects duplicate, foreign or independently re-created catalog input before any paint extraction. */
        bool ValidPreparedSources(const CaptionTimeline &timeline, const std::span<const CaptionPaintCue> prepared) noexcept {
            for (std::size_t i = 0; i < prepared.size(); ++i) {
                const auto &entry = prepared[i];
                const auto active = std::ranges::find_if(timeline.Cues(), [&](const auto &value) {
                    return value.cue.id == entry.measured.cue;
                });
                if (active == timeline.Cues().end() || !entry.catalog || entry.catalog != active->catalog)
                    return false;
                for (std::size_t j = 0; j < i; ++j)
                    if (prepared[j].measured.cue == entry.measured.cue)
                        return false;
            }
            return true;
        }
    }  // namespace

    /** @copydoc CaptionRenderer::CaptionRenderer */
    CaptionRenderer::CaptionRenderer(UiRenderExtractor extractor, const UiRenderSnapshotLimits limits)
        : extractor_(std::move(extractor)), limits_(limits) {
        commands_.reserve(limits.commands);
        runs_.reserve(limits.textRuns);
        glyphs_.reserve(limits.glyphs);
        resources_.reserve(limits.resources);
    }

    /** @copydoc CaptionRenderer::Create */
    Result<CaptionRenderer> CaptionRenderer::Create(const UiRenderExtractorDescriptor &descriptor) {
        auto extractor = UiRenderExtractor::Create(descriptor);
        if (extractor.HasError())
            return Result<CaptionRenderer>::Failure(extractor.ErrorValue());
        if (descriptor.limits.commands < 2 || descriptor.limits.textRuns == 0 || descriptor.limits.glyphs == 0 ||
            descriptor.limits.resources == 0 || descriptor.limits.clips == 0)
            return Result<CaptionRenderer>::Failure(MakeError(UiErrors::CapacityExceeded));
        try {
            return Result<CaptionRenderer>::Success(CaptionRenderer{std::move(extractor).Value(), descriptor.limits});
        } catch (const std::bad_alloc &) {
            return Result<CaptionRenderer>::Failure(MakeError(UiErrors::CaptionCapacityExceeded));
        }
    }

    /** @copydoc CaptionRenderer::AppendCue */
    Result<void> CaptionRenderer::AppendCue(const CaptionPaintCue &cue, const CaptionPlacement &placement, const UiLinearColor text,
                                            const UiLinearColor background) {
        if (cue.runs.size() >= limits_.commands - commands_.size() || cue.runs.size() > limits_.textRuns - runs_.size() ||
            cue.glyphs.size() > limits_.glyphs - glyphs_.size() || cue.resources.size() > limits_.resources - resources_.size())
            return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        for (const auto &glyph : cue.glyphs) {
            if (!glyph.extent.IsValid() || glyph.origin.x < 0 || glyph.origin.y < 0 ||
                static_cast<std::int64_t>(glyph.origin.x) + glyph.extent.width > placement.bounds.extent.width ||
                static_cast<std::int64_t>(glyph.origin.y) + glyph.extent.height > placement.bounds.extent.height)
                return Result<void>::Failure(MakeError(UiErrors::CaptionInvalid));
        }
        for (const auto &run : cue.runs)
            if (run.fontResource >= cue.resources.size() || run.firstGlyph > cue.glyphs.size() ||
                run.glyphCount > cue.glyphs.size() - run.firstGlyph)
                return Result<void>::Failure(MakeError(UiErrors::CaptionInvalid));
        const auto glyphOffset = static_cast<std::uint32_t>(glyphs_.size());
        for (const auto &resource : cue.resources)
            if (resource.role != UiRenderResourceRole::FontFace)
                return Result<void>::Failure(MakeError(UiErrors::RenderResourceReferenceInvalid));
        const auto resourceOffset = static_cast<std::uint32_t>(resources_.size());
        resources_.insert(resources_.end(), cue.resources.begin(), cue.resources.end());
        for (auto glyph : cue.glyphs) {
            glyph.origin.x += placement.bounds.origin.x;
            glyph.origin.y += placement.bounds.origin.y;
            glyphs_.push_back(glyph);
        }
        commands_.push_back({cue.element, placement.bounds, 0, 0, NoUiRenderIndex, 1, UiSolidDraw{background}});
        for (auto run : cue.runs) {
            run.firstGlyph += glyphOffset;
            run.fontResource += resourceOffset;
            run.color = text;
            const auto runIndex = static_cast<std::uint32_t>(runs_.size());
            runs_.push_back(run);
            commands_.push_back({cue.element, placement.bounds, 0, 0, NoUiRenderIndex, 1, UiTextDraw{runIndex}});
        }
        return Result<void>::Success();
    }

    /** @copydoc CaptionRenderer::Render */
    Result<CaptionRenderFrame> CaptionRenderer::Render(const UiElementTree &tree, const UiRenderSnapshotDescriptor &descriptor,
                                                       const UiLogicalRect viewport, const CaptionTimeline &timeline,
                                                       const CaptionAppearance &appearance, const Sha256Digest &localization,
                                                       const std::span<const CaptionPaintCue> prepared,
                                                       const UiAnimationFrameLease *frame) {
        using Output = Result<CaptionRenderFrame>;
        if (prepared.size() > MaximumCaptionCues || !appearance.IsValid() || (appearance.enabled && localization == Sha256Digest{}))
            return Output::Failure(MakeError(UiErrors::CaptionInvalid));
        std::array<CaptionMeasuredCue, MaximumCaptionCues> measured{};
        std::size_t visibleCount = 0;
        if (appearance.enabled) {
            if (!ValidPreparedSources(timeline, prepared))
                return Output::Failure(MakeError(UiErrors::CaptionInvalid));
            for (const auto &active : timeline.Cues()) {
                if (!Visible(active, timeline, appearance))
                    continue;
                const auto text = std::ranges::find_if(prepared, [&](const auto &entry) {
                    return entry.measured.cue == active.cue.id;
                });
                if (text == prepared.end() || text->appearance != appearance || text->localization != localization)
                    return Output::Failure(MakeError(UiErrors::CaptionInvalid));
                measured[visibleCount++] = text->measured;
            }
        }
        auto arranged = ArrangeCaptions(viewport, appearance, std::span(measured).first(visibleCount));
        if (arranged.HasError())
            return Output::Failure(arranged.ErrorValue());
        commands_.clear();
        runs_.clear();
        glyphs_.clear();
        resources_.clear();
        const auto textColor = LinearColor(appearance.textColor);
        auto background = LinearColor(appearance.backgroundColor);
        background.alpha *= appearance.backgroundOpacity;
        for (const auto &placement : std::span(arranged.Value().placements).first(arranged.Value().count)) {
            const auto text = std::ranges::find_if(prepared, [&](const auto &entry) {
                return entry.measured.cue == placement.cue;
            });
            if (const auto appended = AppendCue(*text, placement, textColor, background); appended.HasError())
                return Output::Failure(appended.ErrorValue());
        }
        const std::array transforms{UiLogicalTransform{}};
        const std::array clips{UiClip{arranged.Value().safeArea}};
        const UiRenderProjection projection{commands_, runs_, glyphs_, clips, {}, transforms, resources_, {}};
        auto snapshot = frame ? extractor_.Extract(tree, descriptor, projection, *frame) : extractor_.Extract(tree, descriptor, projection);
        if (snapshot.HasError())
            return Output::Failure(snapshot.ErrorValue());
        return Output::Success({arranged.Value(), std::move(snapshot).Value(), localization});
    }
}  // namespace Horo::Runtime::Ui
