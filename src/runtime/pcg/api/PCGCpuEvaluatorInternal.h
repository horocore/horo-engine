#pragma once

#include "Horo/PCG/PCGCpuEvaluator.h"

#include <algorithm>

namespace Horo::PCG::detail {
    /** @brief Returns a stable disjoint partition boundary for an admitted nonzero worker count. */
    [[nodiscard]] inline std::size_t PartitionEnd(std::size_t count, std::uint32_t workers, std::uint32_t partition) noexcept {
        const std::size_t block = count / workers;
        const std::size_t extra = count % workers;
        return block * (partition + 1U) + std::min<std::size_t>(extra, partition + 1U);
    }

    /** @brief Counts a legacy scalar-density selection; joins every child before borrowed source storage may retire. */
    [[nodiscard]] Result<std::size_t> CountSelected(const PCGPointReadView &source, double threshold, std::uint32_t workers,
                                                    JobSystem *jobs, const CancellationToken &cancellation);

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
