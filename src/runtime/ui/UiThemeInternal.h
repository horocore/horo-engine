#pragma once

#include "Horo/Runtime/Ui/UiTheme.h"

namespace Horo::Runtime::Ui::ThemeInternal {
    /** @brief Restores explicitly declared removed tokens while preserving the active category/sealing contract. */
    [[nodiscard]] Result<void> RestoreTokens(UiStyleRegistryDefinition &definition, const RuntimeStyleRegistry &active,
                                             std::span<const UiThemeTokenFallback> fallbacks);
    /** @brief Checks surface consumer categories and invalidation effects before admitting a registry. */
    [[nodiscard]] Result<void> ValidateProperties(const RuntimeStyleRegistry &registry, const UiThemeSurfaceProperties &properties);
    /** @brief Projects one resolved surface into declarative layout and solid-paint values. */
    [[nodiscard]] Result<void> ProjectSurface(const UiComputedStyleSnapshot &snapshot, const UiThemeSurfaceInput &input,
                                              const UiThemeSurfaceProperties &properties, UiLayoutElementDescriptor &layout,
                                              UiDrawCommand &paint);
}  // namespace Horo::Runtime::Ui::ThemeInternal
