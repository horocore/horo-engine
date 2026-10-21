#pragma once

/**
 * @file RenderGraphSynchronization.h
 * @brief Backend-neutral render-graph resource-state and barrier synthesis contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderGraph.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Access intent carried by synchronization state, including an explicit undefined state. */
    enum class RenderGraphSynchronizationAccess : std::uint8_t {
        None,
        Read,
        Write,
        ReadWrite
    };

    /** @brief Backend-neutral operation class carried by synchronization state. */
    enum class RenderGraphSynchronizationOperation : std::uint8_t {
        None,
        Sampled,
        Storage,
        ColorAttachment,
        DepthStencilAttachment,
        CopySource,
        CopyDestination,
        GraphicsInputBuffer, /**< Read-only uniform, vertex and index inputs; whole-buffer state conservatively covers all three. */
    };

    /** @brief Pipeline scope in which one logical resource access occurs. */
    enum class RenderGraphPipelineScope : std::uint8_t {
        External,
        Graphics,
        Compute,
        Transfer
    };

    /** @brief Backend-neutral texture layout intent. */
    enum class RenderGraphTextureLayout : std::uint8_t {
        NotApplicable,
        ShaderReadOnly,
        General,
        ColorAttachment,
        DepthStencilAttachment,
        CopySource,
        CopyDestination,
    };

    /** @brief Complete logical access state for one whole graph resource. */
    struct RenderGraphLogicalState {
        RenderGraphSynchronizationAccess access{RenderGraphSynchronizationAccess::None};
        RenderGraphSynchronizationOperation operation{RenderGraphSynchronizationOperation::None};
        RenderGraphPipelineScope scope{RenderGraphPipelineScope::External};
        RenderGraphTextureLayout layout{RenderGraphTextureLayout::NotApplicable};
        RenderQueueId queue;

        [[nodiscard]] constexpr auto operator<=>(const RenderGraphLogicalState &) const noexcept = default;
    };

    /** @brief Explicit contents policy for one exact imported resident generation. */
    enum class RenderGraphInitialContents : std::uint8_t {
        Preserved,
        Discarded,
    };

    /** @brief Exact initial state evidence for one imported graph resource generation. */
    struct RenderGraphImportedState {
        RenderGraphResourceId resource;
        RenderGraphLogicalState state;
        RenderGraphInitialContents contents{RenderGraphInitialContents::Preserved};
        /**< Discarded requires the exact undefined state and a dominating first write;
         * it grants no permission to discard queue ownership or overlap prior GPU readers. */
    };

    /** @brief Independent hazard reasons carried as a complete typed bit set. */
    enum class RenderGraphHazard : std::uint8_t {
        None = 0,
        Initial = 1 << 0,
        ReadAfterWrite = 1 << 1,
        WriteAfterRead = 1 << 2,
        WriteAfterWrite = 1 << 3,
        StateChange = 1 << 4,
    };

    [[nodiscard]] constexpr RenderGraphHazard operator|(const RenderGraphHazard left, const RenderGraphHazard right) noexcept {
        return static_cast<RenderGraphHazard>(static_cast<std::uint8_t>(left) | static_cast<std::uint8_t>(right));
    }

    /** @brief Reports whether a hazard set contains one requested reason. */
    [[nodiscard]] constexpr bool HasRenderGraphHazard(const RenderGraphHazard hazards, const RenderGraphHazard requested) noexcept {
        return (static_cast<std::uint8_t>(hazards) & static_cast<std::uint8_t>(requested)) != 0;
    }

    /** @brief One normalized logical transition before a scheduled resource use. */
    struct RenderGraphTransition {
        RenderGraphResourceId resource;
        RenderGraphPassRef before;
        RenderGraphPassRef after;
        RenderGraphLogicalState oldState;
        RenderGraphLogicalState newState;
        RenderGraphHazard hazards{RenderGraphHazard::None};
    };

    /**
     * @brief Pre-submission logical release/acquire edge for a change of effective queue identity.
     *
     * This value does not contain timeline points. Submission partitioning and the exact ADR-173
     * signal/wait values are attached by RND-009.6 (#352), after this deterministic state pass.
     */
    struct RenderGraphOwnershipTransfer {
        RenderGraphResourceId resource;
        RenderGraphPassRef releaseAfter; /**< Producer pass, or invalid for imported boundary ownership. */
        RenderGraphPassRef acquireBefore;
        RenderQueueId sourceQueue;
        RenderQueueId destinationQueue;
    };

    /** @brief Owning deterministic synchronization plan consumed by backend translators. */
    class RenderGraphSynchronizationPlan final {
    public:
        RenderGraphSynchronizationPlan(const RenderGraphSynchronizationPlan &) = delete;
        RenderGraphSynchronizationPlan &operator=(const RenderGraphSynchronizationPlan &) = delete;
        RenderGraphSynchronizationPlan(RenderGraphSynchronizationPlan &&other) noexcept;
        RenderGraphSynchronizationPlan &operator=(RenderGraphSynchronizationPlan &&other) noexcept;
        ~RenderGraphSynchronizationPlan() = default;

        /** @brief Returns the graph owner for which this plan was synthesized, or invalid after move. */
        [[nodiscard]] RenderGraphOwnerId Owner() const noexcept;
        /**
         * @brief Returns the canonical effective queue topology captured during synthesis.
         * @return Immutable role-ordered view valid for the lifetime of this plan.
         */
        [[nodiscard]] std::span<const RenderQueueAssignment> QueueAssignments() const noexcept;
        /** @brief Returns owned exact imported generation states, including explicit discard policy. */
        [[nodiscard]] std::span<const RenderGraphImportedState> ImportedStates() const noexcept;
        /** @brief Returns normalized transitions in scheduled-pass and authored-use order. */
        [[nodiscard]] std::span<const RenderGraphTransition> Transitions() const noexcept;
        /** @brief Returns matched ownership transfers in transition order. */
        [[nodiscard]] std::span<const RenderGraphOwnershipTransfer> OwnershipTransfers() const noexcept;

    private:
        friend Result<RenderGraphSynchronizationPlan> SynthesizeRenderGraphSynchronization(const RenderGraph &, const RenderGraphSchedule &,
                                                                                           std::span<const RenderQueueAssignment>,
                                                                                           std::span<const RenderGraphImportedState>);

        RenderGraphSynchronizationPlan(RenderGraphOwnerId owner, std::vector<RenderQueueAssignment> queueAssignments,
                                       std::vector<RenderGraphTransition> transitions, std::vector<RenderGraphOwnershipTransfer> transfers,
                                       std::vector<RenderGraphImportedState> importedStates) noexcept;

        RenderGraphOwnerId owner_;
        std::vector<RenderQueueAssignment> queueAssignments_;
        std::vector<RenderGraphTransition> transitions_;
        std::vector<RenderGraphOwnershipTransfer> transfers_;
        std::vector<RenderGraphImportedState> importedStates_;
    };

    /**
     * @brief Synthesizes deterministic whole-resource transitions and effective-queue transfers.
     *
     * The operation is synchronous, allocation-bounded by the graph usage capacity, and does not
     * mutate the graph or schedule. Every imported resource must have exactly one initial state;
     * transient resources start undefined. Queue roles are resolved through the supplied effective
     * topology, so aliased roles never create a false ownership transfer. The plan owns that topology
     * in canonical role order so downstream compilation can verify provenance even when no transition
     * is emitted. Conflicting states within one pass are rejected because the plan cannot insert a
     * pass-internal barrier. A transient's
     * first write emits an explicit `None` to access-state transition; an undefined read is rejected
     * earlier by `CompileRenderGraph` with `RenderGraphErrors::ReadBeforeWrite`.
     *
     * Current graph usages do not yet declare buffer or texture subresource ranges, so this stage
     * deliberately normalizes each use to the whole logical resource. It does not silently claim
     * fine-grained tracking. Likewise, current import/export records carry no required final state;
     * this function cannot invent one, and final boundary-state transitions remain deferred until
     * that graph authoring contract exists. Failure publishes no plan.
     *
     * @param graph Immutable finalized graph.
     * @param schedule Valid schedule compiled from `graph`.
     * @param queues Complete effective queue assignment for every retained pass role.
     * @param importedStates Exact initial state evidence for every non-transient resource.
     * @return Owning normalized plan or a typed actionable validation/allocation failure.
     */
    [[nodiscard]] Result<RenderGraphSynchronizationPlan> SynthesizeRenderGraphSynchronization(
        const RenderGraph &graph, const RenderGraphSchedule &schedule, std::span<const RenderQueueAssignment> queues,
        std::span<const RenderGraphImportedState> importedStates);
}  // namespace Horo::Render
