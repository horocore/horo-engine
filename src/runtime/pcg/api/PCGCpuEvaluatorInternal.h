#pragma once

#include "Horo/PCG/PCGCpuEvaluator.h"

namespace Horo::PCG::detail {
    // Borrowed only for the synchronous evaluation; every child job joins before return.
    struct NodeExecutionContext final {
        const PCGCookedPlan &plan;
        const PCGSpatialSnapshot &spatial;
        PCGPointCloudWorkspace &workspace;
        std::span<const PCGPointOutputBound> bounds;
        std::span<const PCGCpuInput> inputs;
        std::uint32_t workers;
        JobSystem *jobs;
        CancellationToken cancellation;
        std::span<const PCGProvenance> provenance;
    };

    [[nodiscard]] Result<std::size_t> AdmitCandidate(const PCGCookedPlan &plan, std::span<const PCGPointOutputBound> bounds,
                                                     const PCGCpuEvaluationLimits &limits);
    [[nodiscard]] Result<void> ExecuteNode(const NodeExecutionContext &context, std::uint32_t node);
}  // namespace Horo::PCG::detail
