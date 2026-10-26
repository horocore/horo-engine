#include "UiPaintProgramInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    /** @copydoc UiPaintSubmissionEvidence::Capture */
    void UiPaintSubmissionEvidence::Capture(const UiRenderSubmission &source) noexcept {
        for (std::size_t index = 0; index < images.size(); ++index) {
            if (index >= source.images.size()) {
                images[index] = {};
                continue;
            }
            const auto &binding = source.images[index];
            images[index] = {binding.registry, binding.image, binding.page, binding.texture, *binding.realization};
        }
        for (std::size_t index = 0; index < fonts.size(); ++index) {
            if (index >= source.fonts.size()) {
                fonts[index] = {};
                continue;
            }
            const auto &binding = source.fonts[index];
            fonts[index] = {binding.texture, *binding.face, *binding.realization};
        }
        imageCount = source.images.size();
        fontCount = source.fonts.size();
        atlasFrame = source.atlas.Frame();
        atlasRevision = source.atlas.Revision();
    }

    /** @copydoc UiPaintSubmissionEvidence::Matches */
    bool UiPaintSubmissionEvidence::Matches(const UiRenderSubmission &source) const noexcept {
        // An unused atlas contributes no encoded binding; canonical retention still validates and owns its lifetime.
        if (source.images.size() != imageCount || source.fonts.size() != fontCount ||
            (fontCount != 0 && (source.atlas.Frame() != atlasFrame || source.atlas.Revision() != atlasRevision)))
            return false;
        for (const auto &binding : source.images) {
            if (!binding.realization)
                return false;
            const auto found = std::find_if(images.begin(), images.begin() + imageCount, [&](const Image &accepted) {
                return accepted.registry == binding.registry && accepted.image == binding.image && accepted.page == binding.page &&
                       accepted.texture == binding.texture && accepted.realization &&
                       accepted.realization->Creation().handle == binding.realization->Creation().handle &&
                       accepted.realization->SourceOwnership().SharesGeneration(binding.realization->SourceOwnership());
            });
            if (found == images.begin() + imageCount)
                return false;
        }
        for (const auto &binding : source.fonts) {
            if (!binding.face || !binding.realization)
                return false;
            const auto found = std::find_if(fonts.begin(), fonts.begin() + fontCount, [&](const Font &accepted) {
                return accepted.texture == binding.texture && accepted.face && accepted.realization &&
                       accepted.face->SharesGeneration(*binding.face) &&
                       accepted.realization->Creation().handle == binding.realization->Creation().handle &&
                       source.atlas.OwnsPage(accepted.realization->Source());
            });
            if (found == fonts.begin() + fontCount)
                return false;
        }
        return true;
    }
}  // namespace Horo::Render::Detail
