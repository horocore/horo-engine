#pragma once
/** @file VfxGpuScheduling.h @brief Borrowed owner-only append plan; no retained live world or native pointer authority. */
#include "VfxGpuStorage.h"

namespace Horo::Render::Detail {
    struct VfxGpuAppendPlan final {
        VfxGpuRecord &record;
        const VfxGpuGraphRequest &request;
        GpuVfxSimulationWork work;
        std::array<std::uint32_t, VfxGpuDispatchWords::Count> parameters{};
        std::array<RenderBufferHandle, 4> handles;
        std::array<RenderGraphResourceId, 4> resources;
        std::size_t step{}, source{}, destination{}, observation{}, draw{};
        RenderGraphPassRef producer;
        bool dispatch{}, replaceUndispatched{};
        std::uint32_t inputCount{};
        VfxGpuGraphSchedule schedule;
    };

    /** @brief Selects existing exact generation/ring slots without mutating metadata or allocating. */
    [[nodiscard]] const ErrorCodeDescriptor *PlanVfxGpu(VfxGpuAppendPlan &plan, RenderGraphOwnerId graph) noexcept;
    /** @brief Commits only metadata corresponding to a completely appended graph transaction. */
    void CommitVfxGpuPlan(VfxGpuAppendPlan &plan, RenderGraphOwnerId graph, std::uint64_t frame) noexcept;
}  // namespace Horo::Render::Detail
