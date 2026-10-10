#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "MetalRecordingActivity.h"

#ifdef __OBJC__
#import <Metal/Metal.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace Horo::Render::Detail {
    /** @brief Fully owned worker payload; heap ownership also survives closing the original native domain. */
    struct MetalRecordedOperation final {
        RenderGraphWorkload workload;
        __strong id<MTLCommandBuffer> commands{nil};
        __strong id<MTLTexture> texture{nil};
        __strong id<MTLBuffer> source{nil};
        __strong id<MTLBuffer> destination{nil};
        __strong id<MTLHeap> sourceHeap{nil};
        __strong id<MTLHeap> destinationHeap{nil};
        __strong id<MTLDrawable> presentationDrawable{nil}; /**< Retains the presentation parent, not merely its texture. */
    };

    /** @brief Owner allocation budget counting sessions until their last worker/GPU reference drains. */
    struct MetalRecordingBudget final {
        static constexpr std::size_t MaximumPasses = 16;
        static constexpr std::size_t MaximumLiveRecordings = 8;
        static constexpr std::size_t MaximumUploads = 64;
        static constexpr std::size_t NativeQueueCapacity = 256;
        static_assert(MaximumLiveRecordings * MaximumPasses + 4 + MaximumUploads < NativeQueueCapacity);
        std::atomic<std::size_t> live{0}; /**< Owner increments; final shared-lease destruction decrements on any thread. */
    };

    /**
     * @brief Native recording capsule with no runtime, registry, presentation-port or lease-pool borrow.
     * @details Owner-only Commit/Close observe CPU release/acquire fences. Workers access only
     * their preallocated command buffer and owned resource/heap references. The frontend lease
     * remains in MetalRuntime, where only owner safe points may release it.
     */
    class MetalParallelRecording final : public IRenderParallelGraphRecording {
    public:
        MetalParallelRecording(FrameToken frame, std::vector<MetalRecordedOperation> operations,
                               std::shared_ptr<MetalRecordingBudget> budget) noexcept;
        ~MetalParallelRecording() override;
        MetalParallelRecording(const MetalParallelRecording &) = delete;
        MetalParallelRecording &operator=(const MetalParallelRecording &) = delete;
        MetalParallelRecording(MetalParallelRecording &&) = delete;
        MetalParallelRecording &operator=(MetalParallelRecording &&) = delete;
        [[nodiscard]] FrameToken Frame() const noexcept override;
        [[nodiscard]] std::size_t PassCount() const noexcept override;
        [[nodiscard]] Result<void> Record(std::size_t index, const CancellationToken &cancellation) override;
        void Cancel() noexcept override;
        /** @brief Reports whether closure has drained all already-entered encoders, never waits. */
        [[nodiscard]] bool Idle() const noexcept;
        /** @brief Checks exact ready records before owner publication, without making GPU completion claims. */
        [[nodiscard]] Result<void> Accept();
        /** @brief Commits ready records in canonical graph order before the owner's final presentation buffer. */
        void Commit() noexcept;
        /** @brief Polls every native record, returning failure on any GPU error, never a CPU-only completion. */
        [[nodiscard]] Result<bool> NativeComplete() const;

    private:
        FrameToken frame_;
        std::vector<MetalRecordedOperation> operations_;
        std::shared_ptr<MetalRecordingBudget> budget_;
        std::array<std::atomic<std::uint8_t>, MetalRecordingBudget::MaximumPasses> states_{};
        MetalRecordingActivity activity_;
        bool accepted_{false}; /**< Render-owner only. */
    };
}  // namespace Horo::Render::Detail
#endif
