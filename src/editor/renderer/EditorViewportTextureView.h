#pragma once

/**
 * @file EditorViewportTextureView.h
 * @brief Internal backend-neutral borrowed viewport GUI binding shared by editor and explicit application hosts.
 */

#include "Horo/Runtime/Render/RenderResource.h"

#include <cstdint>

namespace Horo::Editor {
    /** @brief Backend-supplied GUI texture identity and normalized presentation coordinates. */
    struct EditorViewportTextureView {
        std::uintptr_t textureId{0};
        float u0{0.0F};
        float v0{0.0F};
        float u1{1.0F};
        float v1{1.0F};
        Render::RenderTextureViewHandle sampledView; /**< Exact generation owned by this viewport; native ID alone is insufficient. */
        Render::RenderSamplerHandle sampledSampler;  /**< Optional matching frontend-owned sampler. */

        /** @brief Reports whether the matching GUI bridge can consume this view. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return textureId != 0;
        }
    };

}  // namespace Horo::Editor
