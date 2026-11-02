#pragma once
#include "Horo/Animation/AnimationClip.h"

namespace Horo::Animation::Detail {
    /** @brief Validates and canonicalizes owned immutable event/schema metadata before clip publication. */
    [[nodiscard]] Result<void> ValidateClipEvents(AnimationClipData &data, const AnimationClipLimits &limits);
}  // namespace Horo::Animation::Detail
