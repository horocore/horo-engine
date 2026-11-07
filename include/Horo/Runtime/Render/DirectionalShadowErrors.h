#pragma once
/** @file DirectionalShadowErrors.h @brief Stable typed failures for shadow preparation and native cache acceptance. */
#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Render::DirectionalShadowErrors {
    extern const ErrorCodeDescriptor InvalidProfile, InvalidScene, InvalidCoordinates, InvalidCache;

    /** @var InvalidProfile
     * @brief Unsupported cascade/tile/filter budget or nonfinite/out-of-range split, coverage or bias settings.
     * @details No native resource or prior accepted profile is changed by this validation failure.
     */
    /** @var InvalidScene
     * @brief Invalid scene/camera/light/caster values, aspect ratio, or unsupported clip-depth selection.
     * @details Borrowed scene values remain caller-owned; the failed preparation retains nothing.
     */
    /** @var InvalidCoordinates
     * @brief Localized caster or receiver-fit coordinates cannot be represented within the finite shadow envelope.
     * @details Rebase or reduce the scene's admitted shadow coverage; no identity matrix is substituted for failed fitting.
     */
    /** @var InvalidCache
     * @brief Missing prepared/native identities or exhausted accepted-generation capacity prevents cache publication.
     * @details The caller retains its prior completion state; this error never claims that a native pass completed.
     */
}  // namespace Horo::Render::DirectionalShadowErrors
