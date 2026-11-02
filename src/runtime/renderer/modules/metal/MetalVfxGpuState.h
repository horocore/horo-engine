#pragma once
/** @file MetalVfxGpuState.h @brief Private complete-family and issued simulation journals for native GPU particles. */
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/VfxGpuKernel.h"
#include "MetalResourceInstances.h"

#import <Metal/Metal.h>
#include <array>
#include <memory>
#include <vector>

namespace Horo::Render::Detail {
    struct MetalVfxGpuOwner final {};

    /** @brief Genuine cooked compute and direct billboard pipelines, scoped to one actual runtime incarnation. */
    class MetalVfxGpuKernel final : public IResidentVfxGpuKernel {
    public:
        std::shared_ptr<const MetalVfxGpuOwner> owner;
        std::uint64_t incarnation{};
        __strong id<MTLComputePipelineState> compute{nil};
        __strong id<MTLRenderPipelineState> drawColor{nil}, drawDepth{nil};
        __strong id<MTLDepthStencilState> depthRead{nil}, depthDisabled{nil};
        std::array<NSUInteger, 5> computeBindings{};
        NSUInteger particlesBinding{}, viewBinding{};
    };

    struct MetalVfxGpuBuffer final {
        NativeVfxGpuBuffer issued;
        MetalBufferInstance *native{}; /**< Exact frontend-pinned native generation; never inferred from its numeric handle. */
        std::uint64_t generation{};
        VfxGpuStepIdentity observationStep;
        bool observationHeld{}, observationCompleted{}, observationFailed{};
    };

    /** @brief One actual command transaction; no accepted state is inferred from encoding or graph metadata. */
    struct MetalVfxGpuJournal final {
        VfxGpuStepIdentity step;
        __strong id<MTLCommandBuffer> commands{nil};
        RenderTimelinePoint submission;
        std::size_t source{}, destination{}, observation{};
        bool occupied{}, accepted{}, terminal{}, observe{}, uncertain{};
    };

    /** @brief Holds exact inventory pins until every accepted native use is terminal; arrays are allocated only at preparation. */
    class MetalVfxGpuSimulation final : public IResidentVfxGpuSimulation {
    public:
        ~MetalVfxGpuSimulation() override;
        /** @brief Reports engine-owned native metadata payload; frontend separately charges its opaque registry lease once. */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept override;
        std::shared_ptr<const MetalVfxGpuOwner> owner;
        std::uint64_t incarnation{};
        VfxGpuSimulationId simulationId;
        VfxGpuSimulationDescriptor descriptor;
        std::shared_ptr<MetalVfxGpuKernel> kernel;
        std::shared_ptr<IRenderGraphResourceLease> resources;
        std::vector<MetalVfxGpuBuffer> states, observations;
        MetalVfxGpuBuffer forces;
        std::vector<MetalVfxGpuJournal> journals;
        std::vector<NativeGpuVfxStepReceipt> receipts;
        std::size_t receiptHead{}, receiptCount{}, reservedReceipts{};
        std::uint64_t currentGeneration{1}, nextTick{};
        std::size_t currentSource{};
        bool stopped{};

        [[nodiscard]] bool CanRetire() const noexcept;
        [[nodiscard]] bool QueueReceipt(const NativeGpuVfxStepReceipt &receipt) noexcept;
        void PublishTerminal(MetalVfxGpuJournal &journal, MTLCommandBufferStatus status) noexcept;
        void ObserveCompletions() noexcept;
        void FinishCommands(id<MTLCommandBuffer> commands, bool committed) noexcept;
        void PreserveUncertain(id<MTLCommandBuffer> commands) noexcept;
    };

    /** @brief Fair owner-only bounded scan; live prefixes cannot starve later quiescent inventories.
     * @param simulations Canonical nonnull native simulation inventory.
     * @param cursor Next position, advanced without skipping the successor of an erased item.
     * @param budget Maximum inspected entries, independent of pending GPU progress.
     * @details Actual native retirement still requires unique CPU ownership plus CanRetire command/resource evidence.
     */
    void DrainRetiredMetalVfxGpuSimulations(std::vector<std::shared_ptr<MetalVfxGpuSimulation>> &simulations, std::size_t &cursor,
                                            std::size_t budget) noexcept;
}  // namespace Horo::Render::Detail
