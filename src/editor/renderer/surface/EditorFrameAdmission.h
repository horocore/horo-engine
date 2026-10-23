#pragma once

/** @file EditorFrameAdmission.h
 * @brief Host policy for transient renderer frame admission failures.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::Editor {
    /** @brief Recognizes occupied GPU frame capacity reported by an interactive backend.
     * @param error Failure returned while beginning a renderer frame.
     * @return True when the host may defer rendering and retry on a later frame.
     */
    [[nodiscard]] inline bool IsDeferredFrameAdmission(const Error &error) noexcept {
        return (error.domain.Value() == "horo.render.opengl" && error.code.Value() == "render.opengl.frame_backpressure") ||
               (error.domain.Value() == "horo.render.metal" && error.code.Value() == "render.metal.submission_busy");
    }
}  // namespace Horo::Editor
