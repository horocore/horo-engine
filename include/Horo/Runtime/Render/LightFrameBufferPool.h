#pragma once

/** @file LightFrameBufferPool.h
 * @brief Preparation-allocated finite light-frame slots with completion-aware updates and explicit teardown.
 */
#include "Horo/Runtime/Render/RenderFrontend.h"

#include <array>
#include <thread>

namespace Horo::Render {
    /** @brief Exact complete uploaded slot revision used to author a fresh graph workload. */
    struct UploadedLightFrame final {
        LightFrameBuffers buffers;
        LightCullingDispatch dispatch;
        std::uint64_t revision{};
    };

    /**
     * @brief Exclusive render-owner pool; all CPU metadata and resident buffers are allocated at preparation.
     * @details Slot count is bounded by the selected device's frames-in-flight limit and engine hard limit eight.
     * Update allocates nothing and never waits; an in-use native slot returns typed Pending without mutation.
     * The frontend must outlive the pool. Host shutdown quiesces calls, destroys graph borrowers, then closes
     * the pool on its creating render thread before destroying the frontend. Released handles remain pinned
     * independently by native command completion. No jobs or callbacks borrow the pool.
     */
    class LightFrameBufferPool final {
    public:
        /**
         * @brief Queues finite reusable host-visible storage buffers once at a preparation safe point.
         * @param frontend Selected initialized renderer that outlives the returned pool.
         * @param budget Fixed product/cook bounds and coverage policy for every slot.
         * @param slots Finite frame slots, no more than the selected device's admitted in-flight count.
         * @return Exclusive owned pool or original typed unsupported/admission/allocation failure; partial creation rolls back.
         */
        [[nodiscard]] static Result<std::unique_ptr<LightFrameBufferPool>> Create(RenderFrontend &frontend,
                                                                                  const LightCullingBudget &budget, std::uint32_t slots);
        /** @brief Closes on the owning thread; host must have ended active frames before destruction. */
        ~LightFrameBufferPool();
        LightFrameBufferPool(const LightFrameBufferPool &) = delete;
        LightFrameBufferPool &operator=(const LightFrameBufferPool &) = delete;

        /**
         * @brief Updates one exact native-complete slot; caller selects the slot without implicit policy fallback.
         * @param slot Index less than the admitted slot count.
         * @param lights Canonical validated packed light table.
         * @param clusters Valid admitted cluster grid.
         * @param cancellation Host cancellation observed before all-or-none native publication.
         * @return Exact uploaded buffer/revision/dispatch evidence or original pending, stale, cancellation or other typed failure.
         */
        [[nodiscard]] Result<UploadedLightFrame> Update(std::uint32_t slot, std::span<const PackedRenderLight> lights,
                                                        std::span<const PackedLightCluster> clusters,
                                                        const CancellationToken &cancellation = {});
        /** @brief Releases every owned generation once on the owner thread; GPU pins retire independently.
         * @return Success including repeated close, or original release failure with remaining handles retained for retry.
         * An affinity failure leaves the pool open; an owner-thread close attempt permanently rejects further updates. */
        [[nodiscard]] Result<void> Shutdown();

    private:
        /** @brief Initialize only finite owner metadata; native allocation remains in Create. */
        LightFrameBufferPool(RenderFrontend &frontend, const LightCullingBudget &budget, std::uint32_t slots);
        RenderFrontend *frontend_;
        LightCullingBudget budget_;
        std::uint32_t count_;
        std::thread::id owner_;
        std::array<LightFrameBuffers, 8> slots_{};
        std::array<std::array<ResourceOperationId, 4>, 8> operations_{};
        std::array<std::uint64_t, 8> revisions_{};
        bool closed_{};
    };
}  // namespace Horo::Render
