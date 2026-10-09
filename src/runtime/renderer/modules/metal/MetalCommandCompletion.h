#pragma once
/** @file MetalCommandCompletion.h
 * @brief Private finite native completion observation for explicit idle and teardown.
 */
#ifdef __OBJC__
#import <Metal/Metal.h>
#include <chrono>
#include <thread>

namespace Horo::Render::Detail {
    /** @brief Native terminal completion is distinct from timeout/context abandonment. */
    enum class MetalCommandCompletion {
        Completed,
        Failed,
        TimedOut
    };

    /**
     * @brief Polls exact native command evidence for a positive, finite teardown budget.
     * @param commands Exact last submission on the runtime's serial native queue, or nil.
     * @return Native completion/failure, or timeout without claiming GPU completion.
     * Normal frames never invoke this helper. Teardown abandons the closed native domain
     * on timeout; committed command buffers retain their native references independently.
     */
    [[nodiscard]] inline MetalCommandCompletion ObserveMetalCommandForTeardown(id<MTLCommandBuffer> commands) noexcept {
        if (commands == nil) {
            return MetalCommandCompletion::Completed;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return MetalCommandCompletion::TimedOut;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return commands.status == MTLCommandBufferStatusCompleted ? MetalCommandCompletion::Completed : MetalCommandCompletion::Failed;
    }
}  // namespace Horo::Render::Detail
#endif
