#pragma once

/**
 * @file RenderGraphExecution.h
 * @brief Backend-neutral compiled render-graph execution contract.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

#include <compare>
#include <cstddef>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Contiguous range into one immutable array owned by a compiled execution plan. */
    struct RenderGraphExecutionRange {
        std::size_t offset{0};
        std::size_t count{0};

        /**
         * @brief Reports whether the range lies within an array of the supplied size.
         * @param size Size of the owning array.
         * @return True when both range endpoints are representable and in bounds.
         */
        [[nodiscard]] constexpr bool IsValidFor(const std::size_t size) const noexcept {
            return offset <= size && count <= size - offset;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphExecutionRange &) const noexcept = default;
    };

    /** @brief One retained pass translated into deterministic backend-neutral execution data. */
    struct RenderGraphExecutionPass {
        RenderGraphPassRef pass;
        RenderPassKind kind{RenderPassKind::Graphics};
        RenderQueueId queue;
        RenderGraphExecutionRange usages;
        RenderGraphExecutionRange dependencies;
        RenderGraphExecutionRange transitions;
        RenderGraphExecutionRange releaseTransfers;
        RenderGraphExecutionRange acquireTransfers;
    };

    /**
     * @brief Immutable owning execution form consumed synchronously by every render backend.
     *
     * Passes are in the validated schedule order. Each pass names ranges in the plan's flat
     * usage, dependency, transition, release-transfer, and acquire-transfer arrays. Release records
     * are emitted after encoding the producer pass; dependency, transition, and acquire records are
     * consumed before encoding the dependent pass. Resources preserve graph-local identities and resident bindings.
     * The plan contains no native handles, command encoders, submission timeline values, or
     * backend-specific fallback policy.
     */
    class CompiledRenderGraphExecution final {
    public:
        CompiledRenderGraphExecution(const CompiledRenderGraphExecution &) = delete;
        CompiledRenderGraphExecution &operator=(const CompiledRenderGraphExecution &) = delete;
        CompiledRenderGraphExecution(CompiledRenderGraphExecution &&other) noexcept;
        CompiledRenderGraphExecution &operator=(CompiledRenderGraphExecution &&other) noexcept;
        ~CompiledRenderGraphExecution() = default;

        /**
         * @brief Returns the source graph owner, or invalid for a moved-from plan.
         * @return Process-local graph owner identity.
         */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept;
        /**
         * @brief Returns graph resources in graph-local identity order.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphResource> Resources() const noexcept;
        /**
         * @brief Returns retained passes in deterministic execution order.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphExecutionPass> Passes() const noexcept;
        /**
         * @brief Returns pass-grouped resource uses referenced by pass ranges.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphResourceUsage> Usages() const noexcept;
        /**
         * @brief Returns incoming authored dependency provenance referenced by pass ranges.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphDependency> Dependencies() const noexcept;
        /**
         * @brief Returns pass-grouped logical transitions referenced by pass ranges.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphTransition> Transitions() const noexcept;
        /**
         * @brief Returns ownership releases grouped under their producer pass.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphOwnershipTransfer> ReleaseTransfers() const noexcept;
        /**
         * @brief Returns ownership acquires grouped under their consumer pass.
         * @return Immutable view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderGraphOwnershipTransfer> AcquireTransfers() const noexcept;
        /** @brief Returns owning imported generation state evidence even when no transition was needed. */
        [[nodiscard]] std::span<const RenderGraphImportedState> ImportedStates() const noexcept;

        /** @brief Returns mandatory post-producer external state/ownership operations, never completion receipts.
         * @return Owned immutable release requirements; backends must encode these or reject before submission. */
        [[nodiscard]] std::span<const RenderGraphExternalRelease> ExternalReleases() const noexcept;

    private:
        friend Result<CompiledRenderGraphExecution> CompileRenderGraphExecution(const RenderGraph &, const RenderGraphSchedule &,
                                                                                const RenderGraphSynchronizationPlan &,
                                                                                std::span<const RenderQueueAssignment>);

        struct Storage {
            std::vector<RenderGraphResource> resources;
            std::vector<RenderGraphExecutionPass> passes;
            std::vector<RenderGraphResourceUsage> usages;
            std::vector<RenderGraphDependency> dependencies;
            std::vector<RenderGraphTransition> transitions;
            std::vector<RenderGraphOwnershipTransfer> releaseTransfers;
            std::vector<RenderGraphOwnershipTransfer> acquireTransfers;
            std::vector<RenderGraphImportedState> importedStates;
            std::vector<RenderGraphExternalRelease> externalReleases;
        };

        /** @brief Adopts fully validated, backend-neutral execution storage. */
        CompiledRenderGraphExecution(RenderGraphOwnerId owner, Storage storage) noexcept;

        RenderGraphOwnerId owner_;
        std::vector<RenderGraphResource> resources_;
        std::vector<RenderGraphExecutionPass> passes_;
        std::vector<RenderGraphResourceUsage> usages_;
        std::vector<RenderGraphDependency> dependencies_;
        std::vector<RenderGraphTransition> transitions_;
        std::vector<RenderGraphOwnershipTransfer> releaseTransfers_;
        std::vector<RenderGraphOwnershipTransfer> acquireTransfers_;
        std::vector<RenderGraphImportedState> importedStates_;
        std::vector<RenderGraphExternalRelease> externalReleases_;
    };

    /**
     * @brief Assembles validated graph, schedule, and synchronization data for backend execution.
     *
     * Compilation is synchronous and allocation-bounded by the finalized graph limits. Queue
     * roles resolve only through the supplied effective topology; missing or duplicate role
     * assignments fail without fallback. The result owns every exposed record, so source values
     * may be released after this call. Multi-queue submission partitioning and timeline assignment
     * remain the responsibility of RND-009.6, and native command/barrier translation remains private
     * to each backend. Failure publishes no partial plan.
     *
     * @param graph Intact finalized graph that owns all referenced identities.
     * @param schedule Valid retained-pass schedule compiled from `graph`.
     * @param synchronization Valid synchronization plan synthesized from `graph` and `schedule`.
     * @param queues Effective logical queue assignment for every retained pass role.
     * @return Immutable owning execution plan or a typed actionable failure.
     */
    [[nodiscard]] Result<CompiledRenderGraphExecution> CompileRenderGraphExecution(const RenderGraph &graph,
                                                                                   const RenderGraphSchedule &schedule,
                                                                                   const RenderGraphSynchronizationPlan &synchronization,
                                                                                   std::span<const RenderQueueAssignment> queues);
}  // namespace Horo::Render
