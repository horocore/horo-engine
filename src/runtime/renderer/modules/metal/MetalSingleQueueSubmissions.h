#pragma once
/** @file MetalSingleQueueSubmissions.h @brief Exact frontend points retained against real commands on one native Metal queue. */
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#import <Metal/Metal.h>
#include <vector>

namespace Horo::Render::Detail {
    class MetalSingleQueueSubmissions final {
    public:
        [[nodiscard]] Result<void> Initialize(std::span<const RenderQueueAssignment> assignments, std::uint64_t incarnation);

        /** @brief Queries the canonical initialized journal identity; Clear revokes this readiness. */
        [[nodiscard]] bool IsConfigured() const noexcept {
            return queue_.IsValid() && incarnation_ != 0;
        }

        [[nodiscard]] Result<void> Capture(const RenderGraphExecutionRequest &request, id<MTLCommandBuffer> commands);
        void Commit(id<MTLCommandBuffer> commands) noexcept;
        void Abort(id<MTLCommandBuffer> commands) noexcept;
        [[nodiscard]] Result<bool> Observe(const RenderCpuWait &wait) const;
        void Clear() noexcept;

    private:
        struct Entry final {
            RenderTimelinePoint point;
            __strong id<MTLCommandBuffer> commands{nil};
            bool accepted{};
        };

        /** @brief Existing graph bounds permit two complete scene/UI graphs for every admitted maximum frame slot. */
        static constexpr std::size_t MaximumEntries = RenderGraphSubmissionLimits::MaximumSubmissions * 2 * 8;
        [[nodiscard]] bool KnownWait(RenderTimelinePoint point, id<MTLCommandBuffer> commands,
                                     std::span<const RenderGraphQueueSubmission> earlier) const noexcept;
        [[nodiscard]] Result<void> Validate(const RenderGraphExecutionRequest &request, id<MTLCommandBuffer> commands) const;
        [[nodiscard]] Result<void> ValidatePoint(const RenderQueueSubmission &submission, std::uint64_t order, std::uint64_t signal,
                                                 id<MTLCommandBuffer> commands, std::span<const RenderGraphQueueSubmission> earlier) const;
        [[nodiscard]] bool MakeRoom(std::size_t count) noexcept;
        RenderQueueId queue_;
        std::uint64_t incarnation_{}, lastOrder_{}, lastSignal_{}, expiredThrough_{};
        std::vector<Entry> entries_;
    };
}  // namespace Horo::Render::Detail
