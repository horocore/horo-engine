#include "Horo/Runtime/Ui/UiStyle.h"

#include "UiStyleInternal.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    /** @copydoc UiStyleColor::IsValid */
    bool UiStyleColor::IsValid() const noexcept {
        return std::isfinite(red) && std::isfinite(green) && std::isfinite(blue) && std::isfinite(alpha) && red >= 0.0F && red <= 1.0F &&
               green >= 0.0F && green <= 1.0F && blue >= 0.0F && blue <= 1.0F && alpha >= 0.0F && alpha <= 1.0F &&
               static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(UiStyleColorRole::Status);
    }

    /** @copydoc UiStyleTypography::IsValid */
    bool UiStyleTypography::IsValid() const noexcept {
        return family.IsValid() && weight <= 1000U && stretch >= 50U && stretch <= 200U && style <= 2U && size > 0 && lineHeight > 0;
    }

    /** @copydoc UiStyleImage::IsValid */
    bool UiStyleImage::IsValid() const noexcept {
        return asset.IsValid() && static_cast<std::uint8_t>(fit) <= static_cast<std::uint8_t>(UiStyleImageFit::Tile) &&
               std::all_of(nineSlice.begin(), nineSlice.end(), [](const std::int32_t value) {
            return value >= 0;
        }) && tint.IsValid();
    }

    /** @copydoc UiStyleShape::IsValid */
    bool UiStyleShape::IsValid() const noexcept {
        return radius >= 0 && borderWidth >= 0 && outlineWidth >= 0 && shadowOffsetX >= -1'000'000 && shadowOffsetX <= 1'000'000 &&
               shadowOffsetY >= -1'000'000 && shadowOffsetY <= 1'000'000 && shadowBlur >= 0;
    }

    /** @copydoc UiStyleScalar::IsValid */
    bool UiStyleScalar::IsValid() const noexcept {
        return std::isfinite(value);
    }

    /** @copydoc UiStyleValueCategoryOf */
    UiStyleValueCategory UiStyleValueCategoryOf(const UiStyleValue &value) noexcept {
        return std::visit([](const auto &typed) noexcept {
            using Value = std::decay_t<decltype(typed)>;
            if constexpr (std::is_same_v<Value, UiStyleColor>)
                return UiStyleValueCategory::Color;
            else if constexpr (std::is_same_v<Value, UiStyleDimension>)
                return UiStyleValueCategory::Dimension;
            else if constexpr (std::is_same_v<Value, UiStyleTypography>)
                return UiStyleValueCategory::Typography;
            else if constexpr (std::is_same_v<Value, UiStyleImage>)
                return UiStyleValueCategory::Imagery;
            else if constexpr (std::is_same_v<Value, UiStyleShape>)
                return UiStyleValueCategory::Shape;
            else if constexpr (std::is_same_v<Value, UiStyleScalar>)
                return UiStyleValueCategory::Scalar;
            else if constexpr (std::is_same_v<Value, UiStyleEnumValue>)
                return UiStyleValueCategory::Enum;
            else
                return UiStyleValueCategory::Motion;
        }, value);
    }

    /** @copydoc UiStyleValueSource::Literal */
    UiStyleValueSource UiStyleValueSource::Literal(UiStyleValue value) noexcept {
        UiStyleValueSource source;
        source.literal = std::move(value);
        source.token = {};
        source.referencesToken = false;
        return source;
    }

    /** @copydoc UiStyleValueSource::Token */
    UiStyleValueSource UiStyleValueSource::Token(const UiStyleTokenReference reference) noexcept {
        UiStyleValueSource source;
        source.token = reference;
        source.referencesToken = true;
        return source;
    }

    /** @copydoc UiStyleValueSource::IsValid */
    bool UiStyleValueSource::IsValid() const noexcept {
        return referencesToken ? token.IsValid() : StyleInternal::IsValueValid(literal);
    }

    /** @copydoc UiStylePropertyDescriptor::IsValid */
    bool UiStylePropertyDescriptor::IsValid() const noexcept {
        if (!id.IsValid() || !StyleInternal::IsKnownCategory(category) || UiStyleValueCategoryOf(defaultValue) != category ||
            !StyleInternal::IsValueCompatible(*this, defaultValue))
            return false;
        if (category == UiStyleValueCategory::Scalar &&
            (!std::isfinite(minimumScalar) || !std::isfinite(maximumScalar) || minimumScalar > maximumScalar))
            return false;
        return true;
    }

    /** @copydoc UiVisualStateMask::IsValid */
    bool UiVisualStateMask::IsValid() const noexcept {
        return (bits & static_cast<std::uint16_t>(~StyleInternal::KnownVisualStateBits)) == 0;
    }

    /** @copydoc UiStyleSourceRevisions::IsValid */
    bool UiStyleSourceRevisions::IsValid() const noexcept {
        return document.IsValid() && tree.IsValid() && registry.IsValid() && content.IsValid() && policy.IsValid() && interaction.IsValid();
    }

    /** @copydoc UiStyleResolverDescriptor::IsValid */
    bool UiStyleResolverDescriptor::IsValid() const noexcept {
        return StyleInternal::SameOwner(instance, canvas) && document.IsValid() && elementCapacity > 0 &&
               elementCapacity <= MaximumUiStyleElements && propertyCapacity > 0 && propertyCapacity <= MaximumUiStyleProperties &&
               invalidationCapacity > 0 && invalidationCapacity <= MaximumUiStructuralCommands && concurrentSnapshots >= 2 &&
               concurrentSnapshots <= MaximumUiStyleSnapshotsInFlight && initialRegistryGeneration.IsValid() &&
               initialPublication.IsValid();
    }
}  // namespace Horo::Runtime::Ui
