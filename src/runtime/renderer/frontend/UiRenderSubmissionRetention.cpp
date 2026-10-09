#include "UiRenderSubmissionRetention.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <utility>
#include <variant>

namespace Horo::Render::Detail {
    namespace {
        using namespace Runtime::Ui;

        /** @brief Checks an exact imported texture binding, not merely an arbitrary graph resource ID. */
        [[nodiscard]] bool IsImportedTexture(const RenderGraphResourceId id, const RenderTextureHandle expected,
                                             const std::span<const RenderGraphResource> resources) {
            return std::ranges::any_of(resources, [id, expected](const RenderGraphResource &resource) {
                const auto *texture = std::get_if<RenderTextureHandle>(&resource.binding);
                return resource.id == id && texture != nullptr && texture->IsValid() && *texture == expected;
            });
        }

        /** @brief Compares one owned image page with immutable extraction evidence. */
        [[nodiscard]] bool MatchesImage(const UiRenderResourceReference &reference, const UiImageResourceSnapshot &image,
                                        const std::uint32_t page) {
            if (reference.role != UiRenderResourceRole::Image || !image.IsDrawable())
                return false;
            const auto &source = image.Resource()->Pages()[page];
            return reference.asset == source.dependency.asset && reference.revision.Value() == image.Revision().Value() &&
                   reference.colorSpace == source.colorSpace && reference.sampling == source.sampling &&
                   reference.fallback == image.Resource()->FallbackPolicy() && reference.residency == image.Residency();
        }

        /** @brief Compares copied font provenance with an exact immutable extraction reference. */
        [[nodiscard]] bool MatchesFont(const UiRenderResourceReference &reference, const UiFontFace &face) {
            return reference.role == UiRenderResourceRole::FontFace && face.IsValid() && face.SourceAsset().IsValid() &&
                   face.Revision().IsValid() && reference.asset == face.SourceAsset() &&
                   reference.revision.Value() == face.Revision().Value();
        }

        /** @brief Returns the existing typed provenance failure without inventing a fallback resource. */
        [[nodiscard]] Result<void> InvalidSource() {
            return Result<void>::Failure(MakeError(UiErrors::RenderCompositionInvalid));
        }

        /** @brief Requires the host-issued realization to name this exact imported image page. */
        [[nodiscard]] bool HasImageBindingAuthority(const UiRenderImageBinding &binding,
                                                    const std::span<const RenderGraphResource> resources) {
            return binding.registry != nullptr && binding.realization != nullptr && binding.realization->Image() == binding.image &&
                   binding.realization->Page() == binding.page &&
                   IsImportedTexture(binding.texture, binding.realization->Creation().handle, resources);
        }

        /** @brief Validates all finite submission bounds before borrowing geometry tables or capturing sources. */
        [[nodiscard]] Result<void> ValidateSubmissionBounds(const UiRenderSubmission &submission) {
            if (submission.images.size() > MaximumUiSubmissionImages || submission.fonts.size() > MaximumUiSubmissionFonts)
                return Result<void>::Failure(MakeError(UiErrors::RenderCompositionCapacityExceeded));
            if (submission.geometry == nullptr || !submission.geometry->IsValid() ||
                (!submission.fonts.empty() && !submission.atlas.Frame().IsValid()))
                return InvalidSource();
            if (const auto &snapshot = submission.geometry->SourceSnapshot();
                snapshot.Resources().size() > MaximumUiSubmissionImages + MaximumUiSubmissionFonts ||
                snapshot.Glyphs().size() > MaximumUiSubmissionGlyphs)
                return Result<void>::Failure(MakeError(UiErrors::RenderCompositionCapacityExceeded));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiRenderSubmissionRetention::CaptureImage */
    Result<void> UiRenderSubmissionRetention::CaptureImage(const UiRenderImageBinding &binding, const UiRenderSnapshot &snapshot,
                                                           const std::span<const RenderGraphResource> resources, const std::size_t index) {
        if (!HasImageBindingAuthority(binding, resources))
            return InvalidSource();
        auto acquired = binding.registry->Acquire(binding.image);
        if (acquired.HasError())
            return Result<void>::Failure(acquired.ErrorValue());
        const auto &image = acquired.Value();
        if (!image.IsDrawable() || binding.page >= image.Resource()->Pages().size())
            return InvalidSource();
        if (!image.SharesGeneration(binding.realization->SourceOwnership()) ||
            !MatchesImage(binding.realization->Source(), image, binding.page))
            return InvalidSource();
        if (!std::ranges::any_of(snapshot.Resources(), [&](const UiRenderResourceReference &reference) {
            return MatchesImage(reference, image, binding.page);
        }))
            return InvalidSource();
        images_[index] = std::move(acquired).Value();
        pages_[index] = binding.page;
        return Result<void>::Success();
    }

    /** @copydoc UiRenderSubmissionRetention::CaptureFont */
    Result<void> UiRenderSubmissionRetention::CaptureFont(const UiRenderFontBinding &binding, const UiRenderSnapshot &snapshot,
                                                          const std::span<const RenderGraphResource> resources,
                                                          const UiGlyphAtlas::FrameLease &atlas, const std::size_t index) {
        if (binding.face == nullptr || binding.realization == nullptr ||
            !IsImportedTexture(binding.texture, binding.realization->Creation().handle, resources) ||
            !binding.realization->Source().IsCurrent() || !atlas.OwnsPage(binding.realization->Source()))
            return InvalidSource();
        if (!std::ranges::any_of(snapshot.Resources(), [&](const UiRenderResourceReference &reference) {
            return MatchesFont(reference, *binding.face);
        }))
            return InvalidSource();
        fonts_[index] = *binding.face;
        fontPages_[index] = binding.realization->Page();
        return Result<void>::Success();
    }

    /** @copydoc UiRenderSubmissionRetention::CountSourceMatches */
    std::size_t UiRenderSubmissionRetention::CountSourceMatches(const UiRenderResourceReference &reference) const {
        std::size_t matches{};
        for (std::size_t index = 0; index < images_.size(); ++index)
            if (images_[index] && MatchesImage(reference, *images_[index], pages_[index]))
                ++matches;
        for (const auto &font : fonts_)
            if (font && MatchesFont(reference, *font))
                ++matches;
        return matches;
    }

    /** @copydoc UiRenderSubmissionRetention::ValidateCoverage */
    Result<void> UiRenderSubmissionRetention::ValidateCoverage(const UiRenderSnapshot &snapshot) const {
        for (const auto &reference : snapshot.Resources()) {
            const auto matches = CountSourceMatches(reference);
            if (matches == 0 || (reference.role != UiRenderResourceRole::FontFace && matches != 1))
                return InvalidSource();
        }
        return Result<void>::Success();
    }

    /** @copydoc UiRenderSubmissionRetention::CountGlyphMatches */
    std::size_t UiRenderSubmissionRetention::CountGlyphMatches(const UiRenderResourceReference &reference, const UiPositionedGlyph &glyph,
                                                               const UiGlyphAtlas::FrameLease &atlas) const {
        std::size_t matches{};
        for (std::size_t index = 0; index < fonts_.size(); ++index) {
            if (fonts_[index] && MatchesFont(reference, *fonts_[index]) &&
                atlas.PinsGlyph(*fonts_[index], glyph.glyph, fontPages_[index], glyph.uv))
                ++matches;
        }
        return matches;
    }

    /** @copydoc UiRenderSubmissionRetention::ValidateGlyphCoverage */
    Result<void> UiRenderSubmissionRetention::ValidateGlyphCoverage(const UiRenderSnapshot &snapshot,
                                                                    const UiGlyphAtlas::FrameLease &atlas) const {
        for (const auto &run : snapshot.TextRuns()) {
            const auto &reference = snapshot.Resources()[run.fontResource];
            for (const auto &glyph : snapshot.Glyphs().subspan(run.firstGlyph, run.glyphCount)) {
                if (CountGlyphMatches(reference, glyph, atlas) != 1)
                    return InvalidSource();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiRenderSubmissionRetention::CaptureSources */
    Result<void> UiRenderSubmissionRetention::CaptureSources(const UiRenderSubmission &submission,
                                                             const std::span<const RenderGraphResource> resources) {
        const auto &snapshot = submission.geometry->SourceSnapshot();
        for (std::size_t index = 0; index < submission.images.size(); ++index) {
            if (const auto captured = CaptureImage(submission.images[index], snapshot, resources, index); captured.HasError())
                return captured;
        }
        for (std::size_t index = 0; index < submission.fonts.size(); ++index) {
            if (const auto captured = CaptureFont(submission.fonts[index], snapshot, resources, submission.atlas, index);
                captured.HasError())
                return captured;
        }
        if (const auto covered = ValidateCoverage(snapshot); covered.HasError())
            return covered;
        return ValidateGlyphCoverage(snapshot, submission.atlas);
    }

    /** @copydoc UiRenderSubmissionRetention::Capture */
    Result<void> UiRenderSubmissionRetention::Capture(UiRenderSubmission &submission,
                                                      const std::span<const RenderGraphResource> resources) {
        Release();
        if (const auto bounded = ValidateSubmissionBounds(submission); bounded.HasError())
            return bounded;
        if (const auto captured = CaptureSources(submission, resources); captured.HasError()) {
            Release();
            return captured;
        }
        geometry_ = *submission.geometry;
        atlas_ = std::move(submission.atlas);
        return Result<void>::Success();
    }

    /** @copydoc UiRenderSubmissionRetention::Release */
    void UiRenderSubmissionRetention::Release() noexcept {
        atlas_ = {};
        for (auto &font : fonts_)
            font.reset();
        for (auto &image : images_)
            image.reset();
        geometry_.reset();
    }
}  // namespace Horo::Render::Detail
