#ifndef HORO_EDITOR_VIEWPORT_LIGHTING_H
#define HORO_EDITOR_VIEWPORT_LIGHTING_H

/** @file EditorViewportLighting.h @brief Explicit host lighting recipe and bounded viewport budgets. */
#include "Horo/Runtime/Render/ClusteredLightGrid.h"

namespace Horo::Editor {
    /** @brief Authored host request; concrete backends consume an admitted selection and never demote it themselves. */
    struct EditorViewportLightingProfile final {
        Render::LightPreparationMode requested{Render::LightPreparationMode::ClusteredForwardNative};
        Render::LightPreparationFallback fallback{Render::LightPreparationFallback::AllowBaselineForwardCpu};
        Render::LightCullingBudget budget{static_cast<std::uint32_t>(Render::MaximumSceneLights), 256,
                                          static_cast<std::uint32_t>(Render::MaximumSceneLights), true};
        std::uint32_t gridX{8};
        std::uint32_t gridY{4};
        std::uint32_t gridZ{8};
        std::uint64_t maximumForwardLightTests{8U * 1024U * 1024U}; /**< Finite per-view CPU selection work, including empty-table draws. */

        /** @brief Admits product budgets before native allocation or selection. @return True for an exact finite 3D envelope. */
        [[nodiscard]] bool IsValid() const noexcept {
            return budget.IsValid() && GridFitsBudget() && maximumForwardLightTests > 0 && maximumForwardLightTests <= 64U * 1024U * 1024U;
        }

    private:
        /** @brief Bounds each axis before the volume multiplication so invalid dimensions never wrap. */
        [[nodiscard]] bool GridFitsBudget() const noexcept {
            if (gridX == 0 || gridY == 0 || gridZ == 0 || gridX > budget.maximumClusters || gridY > budget.maximumClusters ||
                gridZ > budget.maximumClusters)
                return false;
            return std::uint64_t{gridX} * gridY * gridZ <= budget.maximumClusters;
        }
    };

    /** @brief Exact observed operation diagnostics; no mutable global provider or per-frame dynamic payload. */
    struct EditorViewportLightingDiagnostics final {
        Render::LightPreparationSelection selection;
        std::uint32_t sourceLightCount{};
        std::uint64_t shadedReferences{};
        std::uint64_t omittedReferences{};
        std::uint64_t completedLightRevision{}; /**< Native totals belong only to this actual completed uploaded table revision. */
        bool gpuDiagnosticsPending{}; /**< True until exact completed membership evidence is available; never an encode-success claim. */
    };
}  // namespace Horo::Editor

#endif  // HORO_EDITOR_VIEWPORT_LIGHTING_H
