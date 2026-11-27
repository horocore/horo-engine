#pragma once

#include "Horo/Runtime/Input.h"

#include <algorithm>

namespace Horo::Input::Detail {
    /** @brief Identifies explicit raw-trigger range normalization policies.
     * @param policy Binding's declared axis policy.
     * @return Whether the policy includes a unipolar source range.
     */
    [[nodiscard]] constexpr bool IsUnipolarAxisPolicy(const DeadzoneKind policy) noexcept {
        return policy == DeadzoneKind::UnipolarFromNegativeOne || policy == DeadzoneKind::UnipolarFromPositiveOne;
    }

    /** @brief Normalizes an explicitly declared raw range before deadzone and threshold processing.
     * @param value Finite source sample in [-1, 1].
     * @param policy Binding's axis policy; ordinary policies preserve the signed range.
     * @return Bounded signed or unipolar source value.
     */
    [[nodiscard]] constexpr float NormalizeAxisRange(const float value, const DeadzoneKind policy) noexcept {
        const float bounded = std::clamp(value, -1.0F, 1.0F);
        if (policy == DeadzoneKind::UnipolarFromNegativeOne)
            return (bounded + 1.0F) * 0.5F;
        if (policy == DeadzoneKind::UnipolarFromPositiveOne)
            return (1.0F - bounded) * 0.5F;
        return bounded;
    }
}  // namespace Horo::Input::Detail
