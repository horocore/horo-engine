#pragma once

/** @file LightSceneExtraction.h
 * @brief Scene-owner adapter producing canonical renderer light values without backend policy.
 */
#include "Horo/Runtime/Render/LightCulling.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Render {
    /** @brief Scene-domain evidence accompanying every extracted canonical light table. */
    struct ExtractedLightTable final {
        Runtime::SceneRuntimeId scene;
        std::uint64_t structuralRevision{};
        std::size_t count{};
    };

    /**
     * @brief Extracts enabled Directional, Point and Spot components into reusable caller scratch.
     * @details Runs synchronously on the scene owner thread; scans at most 65536 slots and
     * 64 ancestors per enabled light. Positions include ancestor scale; directions use proper
     * rotations, and authored range remains world units. Identity packs the exact entity slot and
     * generation within the returned scene domain. Tables from different scene domains must never
     * be combined. Sort order is increasing identity, independent of traversal and backend.
     * Failure publishes no table: scratch may be modified and must be discarded. The function
     * schedules no work and retains no scene/storage references, so cancellation and shutdown
     * require only discarding unpublished scratch and quiescing this synchronous call.
     * @param scene Current borrowed scene, whose owner must outlive the call.
     * @param maximumLights Admitted finite light count, bounded by LightCullingBudget::HardMaximumLights.
     * @param scratch Reusable storage with at least maximumLights entries.
     * @param cancellation Host operation cancellation observed while scanning and resolving ancestors.
     * @return Exact table count and scene provenance, or original scene/math and typed light failures.
     */
    [[nodiscard]] Result<ExtractedLightTable> ExtractSceneLights(Runtime::RuntimeSceneView scene, std::uint32_t maximumLights,
                                                                 std::span<IdentifiedRenderLight> scratch,
                                                                 const CancellationToken &cancellation = {});
}  // namespace Horo::Render
