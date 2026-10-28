#pragma once
/** @file RenderGraphTemplate.h
 * @brief Non-installed cold graph topology and canonical-completion-bound external binding storage.
 */
#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"

namespace Horo::Render {
    class RenderFrontend;

    namespace Detail {
        /** @brief Typed unbound external declaration; it is neither a resident handle nor an executable resource. */
        struct RenderGraphExternalTextureSlot final {
            RenderGraphResourceId resource;
            RenderTextureDescriptor descriptor;
            RenderGraphLogicalState acquired;
            RenderGraphLogicalState requiredFinal;
            std::uint64_t deviceIncarnation{};
        };

        /** @brief Exact current acquisition replacing one declared logical resource. */
        struct RenderGraphExternalTextureBinding final {
            RenderGraphResourceId resource;
            RenderTextureHandle texture;
        };

        /** @brief One cold-declared optional read in a single graphics pass; never an optional write or native resource. */
        struct RenderGraphOptionalRead final {
            RenderGraphResourceId resource;
            RenderGraphUsageKind role;
        };

        /** @brief Storage-use receipt owned exclusively by the SAME canonical graph completion lease.
         * @details Render-owner-thread only, including backend completion polling and shutdown. The shared control block
         * is allocated cold; a queued lease retains it through late completion even if the topology owner is released.
         * No queue, submission counter, callback, lock, or independent retirement authority is introduced.
         */
        class RenderGraphTemplateBindingState final {
        private:
            friend class PreparedRenderGraphTemplate;
            friend class RenderGraphResourceLeasePool;
            bool bound_{};
            bool inFlight_{};
        };

        /** @brief Immutable topology plus preallocated per-frame external binding storage.
         * @details Prepare is lifecycle-only and compiles the existing canonical schedule and synchronization once.
         * Bind performs bounded allocation-free success validation through the exact frontend registry. A slot remains
         * immutable from canonical lease acquisition until that SAME lease releases after scene AND GUI completion.
         * All methods are serialized on the creating render-owner thread. No normal-frame compile or fallback allocation.
         */
        class PreparedRenderGraphTemplate final {
        public:
            PreparedRenderGraphTemplate(const PreparedRenderGraphTemplate &) = delete;
            PreparedRenderGraphTemplate &operator=(const PreparedRenderGraphTemplate &) = delete;
            PreparedRenderGraphTemplate(PreparedRenderGraphTemplate &&) noexcept = default;
            PreparedRenderGraphTemplate &operator=(PreparedRenderGraphTemplate &&) = delete;
            /** @brief Declares one typed external slot on an ordinary builder without inventing a resident identity.
             * @param builder Existing owner-thread open builder; all authored pass/resource identities remain canonical.
             * @param descriptor Exact cold-admitted native allocation structure.
             * @param acquired Expected acquisition tuple, revalidated against every real generation at Bind.
             * @param requiredFinal Exact external release tuple.
             * @param deviceIncarnation Selected immutable backend device incarnation.
             * @return Typed declaration or original authoring/admission failure. It cannot be submitted unbound.
             */
            [[nodiscard]] static Result<RenderGraphExternalTextureSlot> DeclareTexture(RenderGraphBuilder &builder,
                                                                                       const RenderTextureDescriptor &descriptor,
                                                                                       RenderGraphLogicalState acquired,
                                                                                       RenderGraphLogicalState requiredFinal,
                                                                                       std::uint64_t deviceIncarnation);
            /** @brief Compiles topology and allocates every instance before interactive frame admission.
             * @param graph Finalized graph containing exactly the typed external slots and ordinary resident imports.
             * @param slots Exact external declarations issued for this graph owner.
             * @param residentStates Initial evidence for ordinary resident imports; external states come from slots.
             * @param queues Actual selected backend queue topology, never inferred or substituted.
             * @param instances Finite canonical frame-slot capacity, between one and eight.
             * @param optionalReads Closed read-only buffer set for one graphics pass. Empty preserves ordinary fixed uses.
             * Nonempty admits only persistent vertex/index/uniform reads already declared exactly once; topology,
             * attachment writes, queue/order and precompiled synchronization/retention superset remain immutable.
             * @return Cold prepared storage or a typed structural/state/capacity/allocation failure.
             */
            [[nodiscard]] static Result<PreparedRenderGraphTemplate> Prepare(RenderGraph graph,
                                                                             std::span<const RenderGraphExternalTextureSlot> slots,
                                                                             std::span<const RenderGraphImportedState> residentStates,
                                                                             std::span<const RenderQueueAssignment> queues,
                                                                             std::size_t instances,
                                                                             std::span<const RenderGraphOptionalRead> optionalReads = {});
            /** @brief Atomically binds every external slot using exact fresh registry/source generations.
             * @param frontend Same selected render/device owner that imported these acquisitions.
             * @param instance Canonical retired frame slot chosen by the host; an in-flight slot always rejects.
             * @param bindings Complete unique external acquisition set, in declaration order.
             * @return Immutable borrowed execution until the next successful Bind on this slot, or original typed failure.
             * @post Failed validation leaves admitted unqueued storage unsubmitable; precondition rejection never mutates queued storage.
             * Canonical lease acquisition consumes a successful binding once.
             * @pre No active frame: imports and binding precede the one RenderFrameScope. No borrowed plan survives rebind.
             */
            [[nodiscard]] Result<const CompiledRenderGraphExecution *> Bind(const RenderFrontend &frontend, std::size_t instance,
                                                                            std::span<const RenderGraphExternalTextureBinding> bindings);
            /** @brief Binds an exact bounded subset of the cold optional-read declarations without compiling a new graph.
             * @param frontend SAME registry/device owner. @param instance Retired unqueued canonical instance.
             * @param bindings Complete current external texture set. @param reads Unique exact declared resource/role subset.
             * @return Same canonical immutable bound plan or typed failure. Empty selects no optional reads; the
             * three-argument overload selects all. All mandatory uses stay in their original order.
             * @post Every resident superset resource is revalidated, including unused reads touched by retained barriers.
             * No hot allocation/compilation; queued storage cannot mutate. The workload semantic validator still requires
             * exact selected uses. A stale unselected resource rejects and requires lifecycle rebuilding, never bypass.
             */
            [[nodiscard]] Result<const CompiledRenderGraphExecution *> Bind(const RenderFrontend &frontend, std::size_t instance,
                                                                            std::span<const RenderGraphExternalTextureBinding> bindings,
                                                                            std::span<const RenderGraphOptionalRead> reads);

        private:
            PreparedRenderGraphTemplate(RenderGraphOwnerId owner, std::vector<RenderGraphExternalTextureSlot> slots,
                                        std::vector<CompiledRenderGraphExecution> instances) noexcept;
            [[nodiscard]] static CompiledRenderGraphExecution Clone(const CompiledRenderGraphExecution &source);
            [[nodiscard]] static Result<void> ValidateOptionalReads(const RenderGraph &graph,
                                                                    std::span<const RenderGraphOptionalRead> reads);
            [[nodiscard]] Result<void> SelectReads(CompiledRenderGraphExecution &execution, std::span<const RenderGraphOptionalRead> reads);
            [[nodiscard]] Result<void> ValidateResidentSuperset(const RenderFrontend &frontend,
                                                                const CompiledRenderGraphExecution &execution) const;
            [[nodiscard]] static Result<void> ValidateSlots(const RenderGraph &graph,
                                                            std::span<const RenderGraphExternalTextureSlot> slots);
            RenderGraphOwnerId owner_;
            std::thread::id lane_{std::this_thread::get_id()};
            std::vector<RenderGraphExternalTextureSlot> slots_;
            std::vector<CompiledRenderGraphExecution> instances_;

            struct ReadStorage final {
                std::vector<RenderGraphOptionalRead> declarations;
                std::vector<RenderGraphResourceUsage> completeUses;
                std::vector<int> roles;  // Resource-indexed closed role; -1 means mandatory/non-optional.
                std::vector<std::uint8_t> selected;
                std::vector<std::uint8_t> externalSlots;
            };

            ReadStorage reads_;

            bool binding_{}; /**< Owner-lane foreign-source reentry fence, not cross-thread synchronization. */
        };
    }  // namespace Detail
}  // namespace Horo::Render
