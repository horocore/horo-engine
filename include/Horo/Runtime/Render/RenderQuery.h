#pragma once

/**
 * @file RenderQuery.h
 * @brief Backend-neutral bounded asynchronous timestamp-query lifecycle.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/RenderSubmission.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Render {
    /** @brief Generation-safe identity of one timestamp query. */
    struct RenderTimestampQueryId {
        RenderResourceOwnerId renderer; /**< Frontend incarnation that owns the query. */
        std::uint64_t value{0};         /**< Non-zero query-local identity. */

        /** @brief Reports whether both identity components are usable. @return True for a live-shaped identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return renderer.IsValid() && value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderTimestampQueryId &) const noexcept = default;
    };

    /** @brief Observable lifecycle of one asynchronous timestamp query. */
    enum class RenderTimestampQueryState : std::uint8_t {
        Pending,
        Submitted,
        Ready,
        Failed,
        Cancelled,
        TimedOut,
    };

    /** @brief Queue and bounded completion policy for one timestamp query. */
    struct RenderTimestampQueryDescriptor {
        RenderQueueId queue;                 /**< Logical queue whose timestamp domain is sampled. */
        std::chrono::nanoseconds timeout{0}; /**< Positive caller-owned completion deadline. */

        /** @brief Reports whether the queue and timeout are usable. @return True for a valid descriptor. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return queue.IsValid() && timeout.count() > 0;
        }
    };

    /** @brief Finite timestamp-query metadata bound. */
    struct RenderTimestampQueryLimits {
        std::uint32_t maximumRequests{256};

        /** @brief Reports whether the query bound is usable. @return True for usable limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumRequests > 0;
        }
    };

    /** @brief Bounded accounting for timestamp-query records. */
    struct RenderTimestampQuerySnapshot {
        std::uint32_t requestCount{0};
        std::uint32_t submittedCount{0};
        std::uint32_t readyCount{0};
        std::uint32_t terminalCount{0};
        std::uint64_t failedAdmissionCount{0};
        bool supported{false};
        bool acceptingRequests{false};
    };

    /**
     * @brief Completed timestamp query value owned by the caller after acquisition.
     *
     * The backend converts its native timestamp domain to monotonic nanoseconds before
     * publishing the value. No backend timestamp handle or tick type crosses this boundary.
     */
    struct RenderTimestampQueryResult {
        RenderTimestampQueryId id;
        RenderTimelinePoint completion;
        std::chrono::nanoseconds timestamp{0};

        /** @brief Reports whether identity, completion evidence, and value are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return id.IsValid() && completion.IsValid() && timestamp.count() >= 0;
        }
    };

    /**
     * @brief Owns bounded timestamp queries and their owner-thread completion lifecycle.
     *
     * The support flag is the selected backend capability snapshot. Unsupported backends
     * reject admission with a typed error and never silently fall back to CPU time. All
     * lifecycle methods and destruction belong to the creating render-capable owner thread.
     */
    class RenderTimestampQueryQueue final {
    public:
        /**
         * @brief Creates an empty bounded timestamp-query queue.
         * @param renderer Exact owning frontend incarnation.
         * @param supportsTimestampQueries Effective backend capability, without fallback.
         * @param limits Finite query metadata bounds.
         * @return Owned queue or a typed configuration/capacity failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderTimestampQueryQueue>> Create(RenderResourceOwnerId renderer,
                                                                                       bool supportsTimestampQueries,
                                                                                       const RenderTimestampQueryLimits &limits = {});
        ~RenderTimestampQueryQueue();

        RenderTimestampQueryQueue(const RenderTimestampQueryQueue &) = delete;
        RenderTimestampQueryQueue &operator=(const RenderTimestampQueryQueue &) = delete;
        RenderTimestampQueryQueue(RenderTimestampQueryQueue &&) = delete;
        RenderTimestampQueryQueue &operator=(RenderTimestampQueryQueue &&) = delete;

        /** @brief Admits one timestamp query or returns a typed unsupported/capacity failure. */
        [[nodiscard]] Result<RenderTimestampQueryId> Request(const RenderTimestampQueryDescriptor &descriptor);
        /** @brief Returns the current query state. */
        [[nodiscard]] Result<RenderTimestampQueryState> State(RenderTimestampQueryId query) const;
        /** @brief Reports whether a timestamp value can be acquired. */
        [[nodiscard]] Result<bool> IsReady(RenderTimestampQueryId query) const;
        /** @brief Records the exact queue completion point after backend submission succeeds. */
        [[nodiscard]] Result<void> MarkSubmitted(RenderTimestampQueryId query, RenderTimelinePoint completion);
        /** @brief Publishes one backend-converted monotonic timestamp. */
        [[nodiscard]] Result<void> Complete(RenderTimestampQueryId query, std::chrono::nanoseconds timestamp);
        /** @brief Stores an original typed backend failure. */
        [[nodiscard]] Result<void> Fail(RenderTimestampQueryId query, const Error &error);
        /** @brief Requests cooperative cancellation. */
        [[nodiscard]] Result<void> Cancel(RenderTimestampQueryId query);
        /** @brief Records a caller-observed deadline without blocking. */
        [[nodiscard]] Result<void> Timeout(RenderTimestampQueryId query);
        /** @brief Retires cancelled or timed-out submitted work after backend completion evidence. */
        [[nodiscard]] Result<void> Retire(RenderTimestampQueryId query);
        /** @brief Transfers a ready timestamp result and removes its queue record. */
        [[nodiscard]] Result<RenderTimestampQueryResult> Acquire(RenderTimestampQueryId query);
        /** @brief Removes an acknowledged failed, cancelled, or timed-out terminal record. */
        [[nodiscard]] Result<void> Discard(RenderTimestampQueryId query);
        /** @brief Returns bounded query accounting. */
        [[nodiscard]] RenderTimestampQuerySnapshot Snapshot() const noexcept;
        /** @brief Stops new producer admission while allowing outstanding work to retire. */
        void StopAdmission() noexcept;
        /** @brief Clears CPU-side state after all submitted work has retired; idempotent. */
        void Shutdown() noexcept;

    private:
        class Impl;
        explicit RenderTimestampQueryQueue(std::unique_ptr<Impl> implementation) noexcept;
        std::unique_ptr<Impl> implementation_;
    };

    using RenderQueryId = RenderTimestampQueryId;
    using RenderQueryState = RenderTimestampQueryState;
    using RenderQueryDescriptor = RenderTimestampQueryDescriptor;
    using RenderQueryLimits = RenderTimestampQueryLimits;
    using RenderQuerySnapshot = RenderTimestampQuerySnapshot;
    using RenderQueryResult = RenderTimestampQueryResult;
    using RenderQueryQueue = RenderTimestampQueryQueue;
}  // namespace Horo::Render
