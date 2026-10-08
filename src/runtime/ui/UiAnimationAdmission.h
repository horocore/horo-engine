#pragma once

#include "Horo/Runtime/Ui/UiAnimationOwner.h"

namespace Horo::Runtime::Ui::AnimationInternal {
    /** @brief Checks allocation products against explicit owner ceilings before namespace reservation. */
    [[nodiscard]] bool ValidLimits(const UiAnimationLimits &limits) noexcept;
    /** @brief Qualifies inert definitions against actual active retained-tree and registry owners; no callback or publication occurs. */
    [[nodiscard]] Result<void> ValidateDefinitions(const UiAnimationCanvasDefinition &definition, const UiReloadCanvas &canvas,
                                                   const RuntimeStyleRegistry &registry, const UiAnimationLimits &limits);
    /** @brief Binds declared required motion to actual catalog routes and retained-tree subtree ownership without activating a gate. */
    [[nodiscard]] Result<void> ValidateRouteBindings(const UiAnimationCanvasDefinition &definition, const UiReloadCanvas &canvas);
}  // namespace Horo::Runtime::Ui::AnimationInternal
