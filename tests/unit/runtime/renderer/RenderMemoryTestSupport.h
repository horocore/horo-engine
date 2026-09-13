#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

namespace Horo::Render::TestSupport {
    /** @brief Creates the exact dedicated memory plan used by headless resource fakes. */
    [[nodiscard]] inline RenderMemoryCostPlan DedicatedMemoryCost(const std::size_t payloadBytes, const std::uint64_t compatibility) {
        return {.allocationClass = RenderMemoryAllocationClass::Dedicated,
                .compatibility = RenderMemoryCompatibilityId{compatibility},
                .payloadBytes = payloadBytes,
                .requiredBytes = payloadBytes,
                .alignment = 1};
    }

    /** @brief Creates a valid placement matching a backend-provided memory plan. */
    [[nodiscard]] inline RenderMemoryPlacement PlacementFor(const RenderMemoryCostPlan &plan, const std::uint64_t attempt,
                                                            const std::uint64_t pool = 1) {
        return {.pool = {{1}, pool},
                .scope = {1, 1},
                .attempt = {attempt},
                .memoryClass = plan.memoryClass,
                .compatibility = plan.compatibility,
                .provenance = plan.provenance,
                .budgetRevision = 1,
                .payloadBytes = plan.payloadBytes,
                .requiredBytes = plan.requiredBytes,
                .backingBytes = plan.requiredBytes,
                .allocationClass = plan.allocationClass};
    }
}  // namespace Horo::Render::TestSupport
