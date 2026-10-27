#pragma once

/** @file VfxSchedulingStorage.h @brief Private single authority for prepared VFX view orders and draw storage. */
#include "Horo/Runtime/Render/VfxGraphScheduling.h"

#include <limits>

namespace Horo::Render::Detail {
    /** @brief Charges every canonical contiguous material run and every shader-sized chunk exactly once. */
    inline std::uint32_t CountVfxDrawRuns(const VfxDrawOrder &order) noexcept {
        std::uint32_t draws{};
        std::uint32_t previous = std::numeric_limits<std::uint32_t>::max();
        std::uint32_t inDraw{};
        for (const auto &reference : order.instances) {
            if (reference.batch != previous || inDraw == MaximumVfxDrawInstances) {
                ++draws;
                previous = reference.batch;
                inDraw = 0;
            }
            ++inDraw;
        }
        return draws;
    }

    /** @brief One prepared view's six orders and shared sort scratch; workload aliases retain the entire slot. */
    struct VfxSchedulingSlot final {
        std::array<VfxDrawOrder, 6> orders;
        std::vector<VfxDrawReference> scratch;
    };

    /** @brief Render-owner inventory, allocated before scene activation and shared by scheduling and native preparation. */
    struct VfxSchedulingStorage final {
        VfxSchedulingBudget budget;
        std::size_t chargedBytes{};
        std::vector<std::shared_ptr<VfxSchedulingSlot>> slots;
        std::vector<RenderGraphMeshInstance> graphMeshes; /**< Cold-reserved synchronous resolution scratch, not resident authority. */
        std::vector<RenderGraphTargetInstance> graphTargets;
        bool resolvingGraph{false}; /**< Borrowed spans cannot be replaced by same-owner callback reentry. */
    };
}  // namespace Horo::Render::Detail
