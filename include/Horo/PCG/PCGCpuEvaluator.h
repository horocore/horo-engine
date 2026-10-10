#pragma once

/**
 * @file PCGCpuEvaluator.h
 * @brief Pure, bounded CPU dispatch of cooked PCG nodes over immutable spatial inputs.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/PCG/PCGCookedPlan.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "Horo/PCG/PCGProvenance.h"
#include "Horo/PCG/PCGSpatialSnapshot.h"

#include <array>
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

    /** @brief Returns a stable semantic type ID for one supported built-in node.
     * @param kind Exact closed built-in operation.
     * @return Stable type identity or typed unsupported-kind failure. */
    [[nodiscard]] Result<NodeTypeId> PCGCpuNodeType(PCGCpuNodeKind kind);

    /** @brief One exact externally supplied input; absent inputs use the cooked fallback. */
    struct PCGCpuInput final {
        ExposedInputId id{};       /**< Exact cooked binding identity. */
        PCGGraphValue value{};     /**< Owned typed value. */
        std::uint64_t revision{1}; /**< Nonzero owner-issued semantic input revision. */
    };

    /** @brief Admission state captured by the owner before evaluation. */
    enum class PCGCpuAdmission : std::uint8_t {
        Accepting,
        CancellationRequested,
        ShuttingDown
    };

    /** @brief Complete finite resource and scheduling envelope for background or tooling evaluation.
     * @note Evaluation allocates bounded owned storage and synchronously joins structured child work; it is not frame-hot. */
    struct PCGCpuEvaluationLimits final {
        std::size_t maximumScratchBytes{};   /**< Complete scratch ceiling including workspace, jobs, and provenance. */
        std::size_t maximumCandidateBytes{}; /**< Complete immutable candidate ceiling including retained provenance. */
        std::size_t retainedBytes{};         /**< Complete prior operation charge during replacement. */
        std::uint32_t workers{1};            /**< Stable partitions, 1 through 8; multiworker filter work uses jobs. */
        JobSystem *jobs{};                   /**< Borrowed structured scheduler required when workers exceed one. */
        PCGCapabilitySet
            grantedCapabilities{}; /**< Exact host-granted capabilities including an evaluation mode; Validation alone cannot evaluate. */
        Sha256Digest numericProfile{};         /**< Required nonzero fingerprint for profile-deterministic nodes. */
        PCGWorldId world{};                    /**< Stable host-owned world identity. */
        std::array<std::int64_t, 3> cell{};    /**< Exact world-cell scope. */
        std::uint64_t numericPolicyVersion{1}; /**< Nonzero seed/order/numeric policy revision. */
        Sha256Digest providerContent{};        /**< Host digest of canonical immutable provider values; revision alone is insufficient. */
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
        /** @brief Returns node-ordered canonical provenance, including effective inputs and provider truth.
         * @return Immutable roots retained independently of the request and provider lifetime. */
        [[nodiscard]] std::span<const PCGProvenance> Provenance() const noexcept;
        /** @brief Returns the conservative full operation charge for replacement overlap. */
        [[nodiscard]] std::size_t ReservedBytes() const noexcept;

    private:
        friend Result<PCGCpuCandidate> EvaluatePCGCpu(const PCGCookedPlan &, const PCGSpatialSnapshot &,
                                                      std::span<const PCGPointOutputBound>, std::span<const PCGCpuInput>,
                                                      const PCGCpuEvaluationLimits &, PCGCpuAdmission, CancellationToken);

        /** @brief Adopts only a completely evaluated, detached result.
         * @param plan Immutable cooked source of exact generation, source digest and seed; values are copied.
         * @param snapshot Immutable input identity.
         * @param numericProfile Explicit numeric qualification evidence. @param outputs Owned final columns.
         * @param reservedBytes Complete charged operation footprint excluding retained replacement.
         * @param provenance Owned canonical node roots with effective input/provider evidence. */
        PCGCpuCandidate(const PCGCookedPlan &plan, SpatialSnapshotId snapshot, const Sha256Digest &numericProfile,
                        std::vector<PCGCpuPointOutput> outputs, std::size_t reservedBytes, std::vector<PCGProvenance> provenance) noexcept;

        GraphGeneration generation_{};
        Sha256Digest sourceDigest_{};
        std::uint64_t seed_{};
        SpatialSnapshotId snapshot_{};
        Sha256Digest numericProfile_{};
        std::vector<PCGCpuPointOutput> outputs_;
        std::size_t reservedBytes_{};
        std::vector<PCGProvenance> provenance_;
    };

    /**
     * @brief Evaluates supported built-in nodes in cooked order without touching authoritative state.
     * @param plan Immutable canonical executable plan with exact built-in contracts.
     * @param spatial Immutable provider snapshot; required by SnapshotGrid nodes.
     * @param bounds One exact predeclared shape for every PointSet output.
     * @param inputs Distinct typed overrides of cooked exposed-input defaults.
     * @param limits Finite budgets, workers, world/cell/policy, and authoritative canonical provider-content digest.
     * @param admission Owner-captured lifecycle gate.
     * @param cancellation Shared cooperative cancellation observer checked between bounded partitions.
     * @return Complete immutable candidate or typed failure; never a partial candidate.
     * @pre The scheduler outlives the synchronous call; invoke on a background/tooling lane where a structured join is allowed.
     * @post All accepted children have drained before any input borrow is released. No authoritative target state changes.
     */
    [[nodiscard]] Result<PCGCpuCandidate> EvaluatePCGCpu(const PCGCookedPlan &plan, const PCGSpatialSnapshot &spatial,
                                                         std::span<const PCGPointOutputBound> bounds, std::span<const PCGCpuInput> inputs,
                                                         const PCGCpuEvaluationLimits &limits,
                                                         PCGCpuAdmission admission = PCGCpuAdmission::Accepting,
                                                         CancellationToken cancellation = {});
}  // namespace Horo::PCG
