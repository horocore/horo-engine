#pragma once

/**
 * @file PCGCpuEvaluator.h
 * @brief Pure, bounded CPU dispatch of cooked PCG nodes over immutable spatial inputs.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "Horo/PCG/PCGSpatialSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo {
    class JobSystem;
}

namespace Horo::PCG {
    /** @brief Closed built-in node semantics implemented by the CPU evaluator. */
    enum class PCGCpuNodeKind : std::uint8_t {
        SnapshotGrid,
        DensityFilter,
        Merge,
        Forward
    };

    /** @brief Returns a stable semantic type ID for one supported built-in node. */
    [[nodiscard]] Result<NodeTypeId> PCGCpuNodeType(PCGCpuNodeKind kind);

    /** @brief One exact externally supplied input; absent inputs use the cooked fallback. */
    struct PCGCpuInput final {
        ExposedInputId id{};   /**< Exact cooked binding identity. */
        PCGGraphValue value{}; /**< Owned typed value. */
    };

    /** @brief Admission state captured by the owner before evaluation. */
    enum class PCGCpuAdmission : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown
    };

    /** @brief Complete finite resource and scheduling envelope for one evaluation. */
    struct PCGCpuEvaluationLimits final {
        std::size_t maximumScratchBytes{};      /**< Workspace reservation ceiling. */
        std::size_t maximumCandidateBytes{};    /**< Immutable output reservation ceiling. */
        std::size_t retainedBytes{};            /**< Complete prior operation charge during replacement. */
        std::uint32_t workers{1};               /**< Stable partitions, 1 through 8; multiworker filter work uses jobs. */
        JobSystem *jobs{};                      /**< Borrowed structured scheduler required when workers exceed one. */
        PCGCapabilitySet grantedCapabilities{}; /**< Exact host-granted capabilities. */
        Sha256Digest numericProfile{};          /**< Required nonzero fingerprint for profile-deterministic nodes. */
    };

    /** @brief One unconnected final point output, detached from the mutable workspace. */
    struct PCGCpuPointOutput final {
        NodeId node{};                                 /**< Stable authored node. */
        PinId pin{};                                   /**< Stable authored output pin. */
        std::shared_ptr<const PCGPointStorage> points; /**< Immutable captured columns. */
    };

    /** @brief Immutable evaluation result; owns no scene, target, provider, or editor mutation authority. */
    class PCGCpuCandidate final {
    public:
        PCGCpuCandidate(const PCGCpuCandidate &) = default;
        PCGCpuCandidate &operator=(const PCGCpuCandidate &) = default;
        PCGCpuCandidate(PCGCpuCandidate &&) noexcept = default;
        PCGCpuCandidate &operator=(PCGCpuCandidate &&) noexcept = default;

        /** @brief Returns the exact cooked graph generation. */
        [[nodiscard]] GraphGeneration Generation() const noexcept;
        /** @brief Returns exact canonical graph-source content evidence. */
        [[nodiscard]] Sha256Digest SourceDigest() const noexcept;
        /** @brief Returns the authored seed used for point sample derivation. */
        [[nodiscard]] std::uint64_t Seed() const noexcept;
        /** @brief Returns the exact provider snapshot consumed by the evaluator. */
        [[nodiscard]] SpatialSnapshotId Snapshot() const noexcept;
        /** @brief Returns the certified numeric fingerprint, or zero for portable-only plans. */
        [[nodiscard]] Sha256Digest NumericProfile() const noexcept;
        /** @brief Returns authored-node/pin-ordered immutable final outputs. */
        [[nodiscard]] std::span<const PCGCpuPointOutput> Outputs() const noexcept;
        /** @brief Returns the conservative full operation charge for replacement overlap. */
        [[nodiscard]] std::size_t ReservedBytes() const noexcept;

    private:
        friend Result<PCGCpuCandidate> EvaluatePCGCpu(const PCGCookedPlan &, const PCGSpatialSnapshot &,
                                                      std::span<const PCGPointOutputBound>, std::span<const PCGCpuInput>,
                                                      const PCGCpuEvaluationLimits &, PCGCpuAdmission, CancellationToken);

        /** @brief Adopts only a completely evaluated, detached result. */
        PCGCpuCandidate(GraphGeneration generation, Sha256Digest sourceDigest, std::uint64_t seed, SpatialSnapshotId snapshot,
                        Sha256Digest numericProfile, std::vector<PCGCpuPointOutput> outputs, std::size_t reservedBytes) noexcept;

        GraphGeneration generation_{};
        Sha256Digest sourceDigest_{};
        std::uint64_t seed_{};
        SpatialSnapshotId snapshot_{};
        Sha256Digest numericProfile_{};
        std::vector<PCGCpuPointOutput> outputs_;
        std::size_t reservedBytes_{};
    };

    /**
     * @brief Evaluates supported built-in nodes in cooked order without touching authoritative state.
     * @param plan Immutable canonical executable plan with exact built-in contracts.
     * @param spatial Immutable provider snapshot; required by SnapshotGrid nodes.
     * @param bounds One exact predeclared shape for every PointSet output.
     * @param inputs Distinct typed overrides of cooked exposed-input defaults.
     * @param limits Finite scratch/candidate/replacement budgets and worker partition count.
     * @param admission Owner-captured lifecycle gate.
     * @param cancellation Shared cooperative cancellation observer checked between bounded partitions.
     * @return Complete immutable candidate or typed failure; never a partial candidate.
     */
    [[nodiscard]] Result<PCGCpuCandidate> EvaluatePCGCpu(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                         std::span<const PCGPointOutputBound> bounds, std::span<const PCGCpuInput> inputs,
                                                         const PCGCpuEvaluationLimits &limits,
                                                         PCGCpuAdmission admission = PCGCpuAdmission::Accepting,
                                                         CancellationToken cancellation = {});
}  // namespace Horo::PCG
