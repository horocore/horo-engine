#pragma once
#include "Horo/Runtime/Render/ExposureGpuWorkload.h"
#ifdef __OBJC__
#import <Metal/Metal.h>
#include <thread>

namespace Horo::Render::Detail {
    /** @brief Runtime-issued stable identity prevents stale family address reuse after shutdown. */
    struct ExposureGpuKernelOwner final {};

    /** @brief One actual encoded/accepted native output; never inferred from caller metadata. */
    struct ExposureGpuJournalEntry final {
        __strong id<MTLBuffer> buffer{nil};
        __strong id<MTLCommandBuffer> commands{nil};
        ExposureState state;
        bool accepted{};
        std::uint64_t chainStart{};
        bool invalid{};
    };

    /** @brief Actual Metal family with fixed journal; no native handles escape the private target boundary. */
    class ExposureGpuKernel final : public IResidentExposureGpuKernel {
    public:
        std::array<id<MTLComputePipelineState>, 3> pipelines{};
        std::array<std::array<NSUInteger, 5>, 3> bindings{};
        std::shared_ptr<const ExposureGpuKernelOwner> owner;
        std::uint64_t incarnation{};
        std::thread::id lane{std::this_thread::get_id()};
        std::array<ExposureGpuJournalEntry, 8> journal{};
        /** @brief Validates actual accepted predecessor and reserves a free bounded journal slot.
         * @param work Exact workload. @param previous Actual native input buffer. @param output Actual output buffer.
         * @param queue Actual encoding queue. @return Slot or typed stale/capacity failure; performs no GPU wait.
         */
        [[nodiscard]] Result<std::size_t> Prepare(const RenderGraphExposureGpu &work, id<MTLBuffer> previous, id<MTLBuffer> output,
                                                  id<MTLCommandQueue> queue);
        /** @brief Stages actual encoded output. @param slot Reserved slot. @param work Exact metadata.
         * @param output Real buffer. @param commands Real native command buffer.
         */
        void Record(std::size_t slot, const RenderGraphExposureGpu &work, id<MTLBuffer> output, id<MTLCommandBuffer> commands) noexcept;
        /** @brief Resolves native acceptance or abandonment. @param commands Actual command. @param committed Native accepted flag. */
        void Finish(id<MTLCommandBuffer> commands, bool committed) noexcept;

    private:
        /** @brief Finds the latest retained issued state of a view. @param view Actual view. @return Entry or null. */
        [[nodiscard]] const ExposureGpuJournalEntry *Latest(RenderViewId view) const noexcept;
        /** @brief Finds the latest actually accepted native state, preserving it through an unsent replacement.
         * @param view Actual view. @return Accepted same-view entry or null; does not wait for completion.
         */
        [[nodiscard]] const ExposureGpuJournalEntry *LatestAccepted(RenderViewId view) const noexcept;
        /** @brief Propagates observed native command failure through its exact reset chain without waiting. */
        void InvalidateFailedChains() noexcept;
        /** @brief Checks native buffers against all retained live state identities.
         * @param work Exact operation. @param previous Native input. @param output Native output. @return Safe distinct resources.
         */
        [[nodiscard]] bool ResourcesReusable(const RenderGraphExposureGpu &work, id<MTLBuffer> previous,
                                             id<MTLBuffer> output) const noexcept;
        /** @brief Checks strict submitted-view source ordering. @param work New operation. @return Newer than retained view source. */
        [[nodiscard]] bool Increasing(const RenderGraphExposureGpu &work) const noexcept;
        /** @brief Removes only terminal superseded states; newest recorded and latest accepted per-view states remain retained. */
        void Prune() noexcept;
        /** @brief Checks immutable caller metadata against actual recorded native predecessor.
         * @param work Exact workload. @param previous Native input. @param queue Actual queue. @return Valid admitted predecessor.
         */
        [[nodiscard]] bool Predecessor(const RenderGraphExposureGpu &work, id<MTLBuffer> previous,
                                       id<MTLCommandQueue> queue) const noexcept;
    };
}  // namespace Horo::Render::Detail
#endif
