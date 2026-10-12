#pragma once

/** @file LightFrameUpload.h
 * @brief Explicit reusable frame-buffer update admission at a render-owner safe point.
 */
#include "Horo/Runtime/Render/LightFrameLayout.h"
#include "Horo/Runtime/Render/RenderResource.h"

namespace Horo::Render {
    /** @brief Four ready host-visible storage buffers allocated once for one bounded frame slot. */
    struct LightFrameBuffers final {
        RenderBufferHandle lights;
        RenderBufferHandle clusters;
        RenderBufferHandle membership;
        RenderBufferHandle references;
    };

    /** @brief Finite validated CPU payload borrowed only through synchronous update admission. */
    struct LightFrameUpdate final {
        LightCullingBudget budget;
        std::uint64_t revision{}; /**< Strictly increasing per slot; zero is invalid. */
        std::span<const PackedRenderLight> lights;
        std::span<const PackedLightCluster> clusters;
        CancellationToken cancellation;
    };

    /** @brief Frontend-resolved backend instances; native adapters still validate their own resource namespace. */
    struct NativeLightFrameUpdate final {
        std::array<std::uint64_t, 4> instances{};
        LightFrameUpdate update;
    };

    /**
     * @brief Validates a complete finite packed frame and required-coverage policy before native writes.
     * @param update Strictly canonical packed lights, admitted cluster planes and finite product budget.
     * @return Success or typed invalid/capacity/complete-coverage failure; no allocation or mutation.
     * @details Required coverage with fewer references than submitted lights performs bounded CPU pre-admission
     * membership checks; it never discovers overflow by blocking on normal-frame GPU readback.
     */
    [[nodiscard]] Result<void> ValidateLightFrameUpdate(const LightFrameUpdate &update);
}  // namespace Horo::Render
