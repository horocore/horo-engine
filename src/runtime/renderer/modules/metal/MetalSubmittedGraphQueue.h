#pragma once

#include "MetalParallelRecording.h"

#include <array>

namespace Horo::Render::Detail {
    /**
     * @brief Render-owner-only bounded GPU submission retention.
     * @details No callbacks borrow this queue. Poll never waits; leases release only after
     * every recorded buffer and its final owner tail complete, or explicit domain shutdown.
     */
    class MetalSubmittedGraphQueue final {
    public:
        MetalSubmittedGraphQueue() = default;
        MetalSubmittedGraphQueue(const MetalSubmittedGraphQueue &) = delete;
        MetalSubmittedGraphQueue &operator=(const MetalSubmittedGraphQueue &) = delete;
        MetalSubmittedGraphQueue(MetalSubmittedGraphQueue &&) = delete;
        MetalSubmittedGraphQueue &operator=(MetalSubmittedGraphQueue &&) = delete;

        void Initialize(std::uint32_t capacity);
        [[nodiscard]] std::size_t Count() const noexcept;
        [[nodiscard]] std::size_t RecordingCount() const noexcept;
        /** @brief Transfers the active lease/capsule into an already admitted frame slot before commit. */
        void Remember(id<MTLCommandBuffer> commands, IRenderGraphResourceLease *&lease, std::shared_ptr<MetalParallelRecording> &recording);
        [[nodiscard]] Result<void> Poll();
        /** @brief Closes frontend ownership before its registry is destroyed, even after teardown timeout. */
        void ReleaseLeases() noexcept;
        /** @brief Drops native queue references only after resource-domain shutdown. */
        void Clear() noexcept;

    private:
        [[nodiscard]] Result<bool> OldestComplete() const;
        void RetireOldest() noexcept;

        __strong NSMutableArray<id<MTLCommandBuffer>> *commands_{nil};
        std::array<IRenderGraphResourceLease *, 3> leases_{};
        std::array<std::shared_ptr<MetalParallelRecording>, 3> recordings_{};
    };
}  // namespace Horo::Render::Detail
