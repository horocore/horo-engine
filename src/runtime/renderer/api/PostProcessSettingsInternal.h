#pragma once

#include "Horo/Runtime/Render/PostProcessSettings.h"

namespace Horo::Render::Detail {
    /** @brief Validates explicit override modes and only the values they use. */
    [[nodiscard]] Result<void> ValidatePostProcessOverrides(const PostProcessSettingsOverrides &overrides);
    /** @brief Applies an admitted group override with a finite [0,1] spatial weight. */
    void BlendPostProcessOverrides(PostProcessSettings &settings, const PostProcessSettingsOverrides &overrides, float weight) noexcept;
    /** @brief Resolves a profile underneath explicitly authored volume overrides. */
    [[nodiscard]] PostProcessSettingsOverrides ResolvePostProcessProfile(const PostProcessSettings &profile,
                                                                         const PostProcessSettingsOverrides &overrides) noexcept;
}  // namespace Horo::Render::Detail
