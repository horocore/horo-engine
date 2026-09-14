#pragma once

/**
 * @file StandardPbrPassPlan.h
 * @brief Backend-neutral standard PBR opaque and depth pass planning contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/StandardPbrMaterial.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Frontend-selected raster family whose native realization remains backend-owned. */
    enum class StandardPbrRasterFamily : std::uint8_t {
        Forward,
        ClusteredForward,
        Deferred,
    };

    /** @brief Ordered logical stage in a standard PBR opaque/depth plan. */
    enum class StandardPbrPassStage : std::uint8_t {
        Depth,
        ForwardColor,
        DeferredGBuffer,
        DeferredLighting,
    };

    /** @brief Explicit products written by one logical pass batch. */
    struct StandardPbrPassOutputs {
        bool sceneColor{false};
        bool depth{false};
        bool motionVectors{false};

        [[nodiscard]] constexpr auto operator<=>(const StandardPbrPassOutputs &) const noexcept = default;
    };

    /** @brief Finite frontend policy used to build one immutable pass plan. */
    struct StandardPbrPassPlanRequest {
        static constexpr std::size_t HardMaxMaterials = 1U << 20U;

        StandardPbrRasterFamily family{StandardPbrRasterFamily::Forward};
        std::uint64_t recipeGeneration{0};
        std::size_t maximumMaterials{65'536};
        bool depthPrepass{false};
        bool motionVectors{false};

        /** @brief Validates non-zero generation and finite material capacity. @return True when the request is structurally valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return recipeGeneration != 0 && maximumMaterials > 0 && maximumMaterials <= HardMaxMaterials;
        }
    };

    /** @brief Exact generation-safe material-table reference consumed by planned batches. */
    struct StandardPbrPassMaterial {
        MaterialRuntimeId id;
        std::uint64_t sourceRevision{0};
        std::uint64_t generation{0};
        RenderPipelineHandle pipeline;
        MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};

        [[nodiscard]] constexpr auto operator<=>(const StandardPbrPassMaterial &) const noexcept = default;
    };

    /**
     * @brief One ordered batch referencing a contiguous range of the plan-owned canonical material table.
     * @details Masked depth and color batches reference the same range, which prevents alpha-coverage membership drift.
     */
    struct StandardPbrPassBatch {
        StandardPbrPassStage stage{StandardPbrPassStage::Depth};
        MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
        std::size_t firstMaterial{0};
        std::size_t materialCount{0};
        StandardPbrPassOutputs outputs;
    };

    /** @brief Immutable owned logical work plan consumed by any conforming raster backend. */
    struct StandardPbrPassPlan {
        StandardPbrRasterFamily family{StandardPbrRasterFamily::Forward};
        std::uint64_t recipeGeneration{0};
        StandardPbrPassOutputs requiredOutputs;
        std::vector<StandardPbrPassMaterial> materials;
        std::vector<StandardPbrPassBatch> batches;
    };

    /**
     * @brief Builds the standard PBR opaque/depth work plan for an already selected raster family.
     * @details Performs bounded synchronous work on the caller thread and retains no callbacks, jobs, native handles, or ambient state.
     * The returned value wholly owns its canonical material references. Failure publishes no partial plan; cancellation and shutdown are
     * not applicable to this finite preparation step. Clustered Forward requires a depth prepass. Transparent and additive materials are
     * deliberately rejected because their ordering and blending belong to separate frontend policy.
     * @param request Selected raster policy, generation, products, and finite capacity.
     * @param materials Immutable resident standard PBR materials to admit to opaque/depth work.
     * @return A deterministic backend-neutral plan, or a stable StandardPbrPassPlanErrors failure.
     */
    [[nodiscard]] Result<StandardPbrPassPlan> PrepareStandardPbrPassPlan(const StandardPbrPassPlanRequest &request,
                                                                         std::span<const ResidentStandardPbrMaterial> materials);
}  // namespace Horo::Render
