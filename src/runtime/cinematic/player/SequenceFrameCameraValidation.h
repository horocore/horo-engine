#pragma once

/** @file SequenceFrameCameraValidation.h
 * @brief Target-private camera-key admission for the canonical frame plan.
 */
#include "Horo/Cinematic/SequenceEvaluation.h"

namespace Horo::Cinematic::Detail {
    /** @brief Validates transition vocabulary and duration after key time admission.
     * @param cut Authored camera key. @param remaining Non-negative time remaining in the sequence.
     * @return Whether the transition fits its vocabulary and sequence boundary. */
    [[nodiscard]] inline bool IsValidCameraTransition(const SequenceFrameCameraCutKey &cut, const SequenceTime remaining) noexcept {
        return cut.transitionTier < CameraTransitionTier::Count && cut.fallback < CameraTransitionFallback::Count &&
               cut.transitionCurve < CameraBlendCurve::Count && cut.transitionDuration >= 0 && cut.transitionDuration <= remaining &&
               ((cut.transitionTier == CameraTransitionTier::HardCut) == (cut.transitionDuration == 0));
    }

    /** @brief Validates camera identity and exact time before subtracting from the sequence duration.
     * @param cut Authored camera key. @param duration Validated positive sequence duration.
     * @return Whether the complete camera key is admissible without signed subtraction overflow. */
    [[nodiscard]] inline bool IsValidCameraCut(const SequenceFrameCameraCutKey &cut, const SequenceTime duration) noexcept {
        if (cut.time < 0 || cut.time > duration)
            return false;
        return cut.track.IsValid() && cut.key.IsValid() && cut.camera.IsValid() && IsValidCameraTransition(cut, duration - cut.time);
    }
}  // namespace Horo::Cinematic::Detail
