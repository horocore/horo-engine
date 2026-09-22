#include "Horo/Runtime/Ui/UiGlyphAtlas.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>

namespace Horo::Runtime::Ui {
    namespace {
        [[nodiscard]] constexpr bool IsKnown(const UiGlyphAtlasRasterFormat value) noexcept {
            return value < UiGlyphAtlasRasterFormat::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const UiGlyphAtlasResolutionState value) noexcept {
            return value < UiGlyphAtlasResolutionState::Count;
        }

        [[nodiscard]] constexpr std::size_t BytesPerPixel(const UiGlyphAtlasRasterFormat format) noexcept {
            using enum UiGlyphAtlasRasterFormat;
            switch (format) {
                case Alpha8:
                    return 1;
                case Rgba8:
                    return 4;
                case Count:
                    break;
            }
            return 0;
        }

        [[nodiscard]] bool UnitInterval(const float value) noexcept {
            return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
        }

        [[nodiscard]] bool ValidUv(const std::array<float, 4> &uv) noexcept {
            return std::ranges::all_of(uv, UnitInterval) && uv[0] <= uv[2] && uv[1] <= uv[3];
        }
    }  // namespace

    /** @copydoc UiGlyphAtlasVariant::IsValid */
    bool UiGlyphAtlasVariant::IsValid() const noexcept {
        return scale.IsValid();
    }

    /** @copydoc UiGlyphAtlasGlyphKey::IsValid */
    bool UiGlyphAtlasGlyphKey::IsValid() const noexcept {
        return face.IsValid() && variant.IsValid();
    }

    /** @copydoc UiGlyphAtlasPlacement::IsValid */
    bool UiGlyphAtlasPlacement::IsValid() const noexcept {
        return page.IsValid() && pixels.IsValid() && ValidUv(uv);
    }

    /** @copydoc UiGlyphAtlasDescriptor::IsValid */
    bool UiGlyphAtlasDescriptor::IsValid() const noexcept {
        if (!ownership.IsValid() || !pageExtent.IsValid() || !tileExtent.IsValid() || !IsKnown(format) || !fallback.IsValid() ||
            !limits.IsValid() || !initialRevision.IsValid() || pageExtent.width % tileExtent.width != 0 ||
            pageExtent.height % tileExtent.height != 0)
            return false;

        const auto columns = static_cast<std::uint64_t>(pageExtent.width / tileExtent.width);
        const auto rows = static_cast<std::uint64_t>(pageExtent.height / tileExtent.height);
        if (const auto tiles = columns * rows; columns == 0 || rows == 0 || tiles == 0 || tiles > MaximumUiGlyphAtlasTilesPerPage)
            return false;

        const auto bytesPerPixel = BytesPerPixel(format);
        const auto pageBytes = static_cast<std::uint64_t>(pageExtent.width) * pageExtent.height * bytesPerPixel;
        const auto tileBytes = static_cast<std::uint64_t>(tileExtent.width) * tileExtent.height * bytesPerPixel;
        return bytesPerPixel != 0 && pageBytes > 0 && pageBytes <= MaximumUiGlyphAtlasPageBytes && tileBytes > 0 &&
               tileBytes <= limits.maximumPendingUploadBytes;
    }

    /** @copydoc UiGlyphAtlasRasterData::IsValid */
    bool UiGlyphAtlasRasterData::IsValid() const noexcept {
        if (const auto bytesPerPixel = BytesPerPixel(format); !key.IsValid() || !IsKnown(format) || width == 0 || height == 0 ||
                                                              rowBytes == 0 || bytesPerPixel == 0 ||
                                                              static_cast<std::uint64_t>(width) * bytesPerPixel > rowBytes)
            return false;
        const auto expectedBytes = static_cast<std::uint64_t>(rowBytes) * height;
        return expectedBytes > 0 && expectedBytes <= MaximumUiGlyphAtlasPendingUploadBytes && bytes.size() == expectedBytes;
    }

    /** @copydoc UiGlyphAtlasUploadDescriptor::IsValid */
    bool UiGlyphAtlasUploadDescriptor::IsValid() const noexcept {
        return upload.IsValid() && entry.IsValid() && key.IsValid() && placement.IsValid() && revision.IsValid() && IsKnown(format) &&
               rowBytes > 0 && byteCount > 0;
    }

    /** @copydoc UiGlyphAtlasLookup::IsValid */
    bool UiGlyphAtlasLookup::IsValid() const noexcept {
        return requested.IsValid() && resolved.IsValid() && entry.IsValid() && placement.IsValid() && revision.IsValid() && IsKnown(state);
    }
}  // namespace Horo::Runtime::Ui
