#include "UiStyleInternal.h"
#include "UiThemeInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui::ThemeInternal {
    /** @copydoc ValidateProperties */
    Result<void> ValidateProperties(const RuntimeStyleRegistry &registry, const UiThemeSurfaceProperties &properties) {
        const std::array ids{properties.width, properties.height, properties.padding, properties.fill, properties.opacity};
        using enum UiStyleValueCategory;
        constexpr std::array categories{Dimension, Dimension, Dimension, Color, Scalar};
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (!ids[index].IsValid())
                continue;
            const auto *property = StyleInternal::FindProperty(registry.Properties(), ids[index]);
            if (property == nullptr)
                return StyleInternal::Failure(UiErrors::StyleReferenceInvalid);
            if (property->category != categories[index] || (index < 3 && (property->allowsSignedDimension || !property->effects.measure)) ||
                (index >= 3 && !property->effects.paint) ||
                (index == 4 && (property->minimumScalar < 0.0F || property->maximumScalar > 1.0F)))
                return StyleInternal::Failure(UiErrors::StyleTypeMismatch);
        }
        return Result<void>::Success();
    }

    /** @copydoc ProjectSurface */
    Result<void> ProjectSurface(const UiComputedStyleSnapshot &snapshot, const UiThemeSurfaceInput &input,
                                const UiThemeSurfaceProperties &properties, UiLayoutElementDescriptor &layout, UiDrawCommand &paint) {
        const auto record = snapshot.Get(input.style.element);
        if (record.HasError())
            return Result<void>::Failure(record.ErrorValue());
        layout = {input.style.element, input.layout, input.intrinsic};
        paint = {.element = input.style.element, .opacity = input.opacity, .payload = UiSolidDraw{input.fill}};
        for (const auto &property : snapshot.Properties(record.Value())) {
            if (property.property == properties.width)
                layout.style.width = UiLength::Dip(std::get<UiStyleDimension>(property.value).value);
            if (property.property == properties.height)
                layout.style.height = UiLength::Dip(std::get<UiStyleDimension>(property.value).value);
            if (property.property == properties.padding) {
                const auto value = std::get<UiStyleDimension>(property.value).value;
                layout.style.padding = {value, value, value, value};
            }
            if (property.property == properties.fill) {
                const auto color = std::get<UiStyleColor>(property.value);
                paint.payload = UiSolidDraw{{color.red, color.green, color.blue, color.alpha}};
            }
            if (property.property == properties.opacity)
                paint.opacity = std::get<UiStyleScalar>(property.value).value;
        }
        if (!layout.IsValid() || !std::get<UiSolidDraw>(paint.payload).color.IsValid() || !UiStyleScalar{paint.opacity}.IsValid() ||
            paint.opacity < 0.0F || paint.opacity > 1.0F)
            return StyleInternal::Failure(UiErrors::StyleInvalid);
        return Result<void>::Success();
    }

    /** @copydoc RestoreTokens */
    Result<void> RestoreTokens(UiStyleRegistryDefinition &definition, const RuntimeStyleRegistry &active,
                               const std::span<const UiThemeTokenFallback> fallbacks) {
        if (fallbacks.size() > MaximumUiStyleTokens || definition.assets.size() > MaximumUiStyleAssets)
            return StyleInternal::Failure(UiErrors::CapacityExceeded);
        for (std::size_t index = 0; index < fallbacks.size(); ++index) {
            const auto &fallback = fallbacks[index];
            for (std::size_t previous = 0; previous < index; ++previous)
                if (fallbacks[previous].token == fallback.token)
                    return StyleInternal::Failure(UiErrors::StyleInvalid);
            if (!fallback.token.IsValid() || !active.HasToken(fallback.token) || !StyleInternal::IsValueValid(fallback.value))
                return StyleInternal::Failure(UiErrors::StyleReferenceInvalid);
            const auto *old = StyleInternal::FindToken(active.Assets(), fallback.token);
            if (old == nullptr || old->sealed || old->category != UiStyleValueCategoryOf(fallback.value))
                return StyleInternal::Failure(UiErrors::StyleTypeMismatch);
            const auto asset = std::ranges::find_if(definition.assets, [&fallback](const auto &entry) {
                return entry.id == fallback.token.asset;
            });
            if (asset == definition.assets.end() || StyleInternal::FindToken(*asset, fallback.token.id) != nullptr)
                return StyleInternal::Failure(UiErrors::StyleReferenceInvalid);
            if (asset->tokens.size() >= MaximumUiStyleTokens)
                return StyleInternal::Failure(UiErrors::CapacityExceeded);
            asset->tokens.emplace_back(fallback.token.id, old->category, UiStyleValueSource::Literal(fallback.value), false);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::ThemeInternal
