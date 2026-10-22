#pragma once

/**
 * @file RenderSubmission.h
 * @brief Backend-neutral queue, submission-order, and GPU-completion contracts.
 */

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <span>

namespace Horo::Render {
    /** @brief Logical queue role requested without exposing a native queue family or handle. */
    enum class RenderQueueRole : std::uint8_t {
        Graphics,
        Compute,
        Transfer,
    };

    /**
     * @brief Reports whether a queue role belongs to the public contract.
     * @param role Queue role to validate.
     * @return True for a declared backend-neutral role.
     */
    [[nodiscard]] constexpr bool IsRenderQueueRoleValid(const RenderQueueRole role) noexcept {
        return static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(RenderQueueRole::Transfer);
    }

    /** @brief Frontend-issued identity for one logical queue in an effective device topology. */
    struct RenderQueueId {
        std::uint32_t value{0}; /**< Non-zero identity within one device lifetime. */

        /**
         * @brief Reports whether the identity was issued by a frontend.
         * @return True for a non-zero frontend-issued identity.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderQueueId &) const noexcept = default;
    };

    /**
     * @brief One resolved logical queue; multiple roles may deliberately name the same queue.
     *
     * A single-queue backend publishes the same queue identity for all supported roles.
     * A multi-queue backend publishes distinct identities only after capability admission.
     */
    struct RenderQueueAssignment {
        RenderQueueRole role{RenderQueueRole::Graphics}; /**< Workload role resolved by the frontend. */
        RenderQueueId queue;                             /**< Effective logical queue for the role. */

        /**
         * @brief Reports whether both the role and frontend-issued queue identity are valid.
         * @return True when the role is known and the queue identity is non-zero.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return IsRenderQueueRoleValid(role) && queue.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderQueueAssignment &) const noexcept = default;
    };

    /**
     * @brief Frontend-issued total order for admitted submissions in one device lifetime.
     *
     * Zero is invalid. The maximum value is reserved so incrementing code must fail before
     * wraparound rather than reusing an order value.
     */
    struct RenderSubmissionOrder {
        std::uint64_t value{0}; /**< Non-zero total-order value within one device lifetime. */

        /**
         * @brief Reports whether the order value is non-zero and incrementable.
         * @return True when the value is neither a sentinel nor the reserved wrap boundary.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && value != std::numeric_limits<std::uint64_t>::max();
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderSubmissionOrder &) const noexcept = default;
    };

    /**
     * @brief Completion point on one logical queue timeline.
     *
     * Values are strictly increasing per queue. Zero is invalid and the maximum value is
     * reserved so a backend must report exhaustion before timeline wraparound.
     */
    struct RenderTimelinePoint {
        RenderQueueId queue;    /**< Logical queue whose progress this point observes. */
        std::uint64_t value{0}; /**< Non-zero monotonic value within the queue lifetime. */

        /**
         * @brief Reports whether the queue and incrementable timeline value are valid.
         * @return True when both components are valid and the value cannot wrap on its next issue.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return queue.IsValid() && value != 0 && value != std::numeric_limits<std::uint64_t>::max();
        }

        [[nodiscard]] constexpr auto operator<=>(const RenderTimelinePoint &) const noexcept = default;
    };

    /**
     * @brief One deterministic queue submission with GPU-side waits and one completion signal.
     *
     * `waits` is borrowed synchronously while a compiled plan is admitted. The compiled
     * RenderExecutionPlan or RenderGraph that owns its backing storage must outlive every
     * submission view and the complete synchronous backend-admission call. A wait on the
     * submission's own queue must precede its signal; cross-queue ordering is expressed only
     * through explicit timeline points. The backend must not reorder descriptors by queue.
     */
    struct RenderQueueSubmission {
        RenderQueueId queue;                        /**< Logical queue that executes this submission. */
        RenderSubmissionOrder order;                /**< Frontend-issued total admission order. */
        std::span<const RenderTimelinePoint> waits; /**< Borrowed points whose plan-owned storage must outlive backend admission. */
        RenderTimelinePoint signal;                 /**< Completion point signaled on this submission's queue. */

        /**
         * @brief Validates identities, signal ownership, and same-queue timeline direction.
         * @return True when the submission can be admitted without changing its declared ordering.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            if (!queue.IsValid() || !order.IsValid() || !signal.IsValid() || signal.queue != queue) {
                return false;
            }
            for (const RenderTimelinePoint wait : waits) {
                if (!wait.IsValid() || (wait.queue == queue && wait.value >= signal.value)) {
                    return false;
                }
            }
            return true;
        }
    };

    /** @brief Explicit reason a CPU caller is permitted to observe or block for GPU completion. */
    enum class RenderCpuWaitPurpose : std::uint8_t {
        Poll,
        BoundedReadback,
        DeterministicTest,
        Teardown,
        Recovery,
    };

    /**
     * @brief Finite CPU wait policy for one queue timeline point.
     *
     * Polling uses a zero timeout. Every blocking purpose requires a positive finite timeout;
     * ordinary frame submission has no blocking purpose and must use GPU-side waits instead.
     */
    struct RenderCpuWait {
        RenderTimelinePoint completion;                           /**< GPU completion point to observe. */
        RenderCpuWaitPurpose purpose{RenderCpuWaitPurpose::Poll}; /**< Authorized observation reason. */
        std::chrono::nanoseconds timeout{0};                      /**< Zero for polls; positive for bounded waits. */
        std::uint64_t expectedDeviceIncarnation{0};

        /**< Required by backend timeline observation: exact selected capability incarnation.
         * Owner-bound readback/query ports already validate their own owner handles and may leave this zero. */

        /**
         * @brief Reports whether the completion point, purpose, and timeout agree.
         * @return True for zero-time polling or a declared blocking purpose with a positive timeout.
         */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            if (!completion.IsValid() || timeout.count() < 0) {
                return false;
            }
            switch (purpose) {
                case RenderCpuWaitPurpose::Poll:
                    return timeout.count() == 0;
                case RenderCpuWaitPurpose::BoundedReadback:
                case RenderCpuWaitPurpose::DeterministicTest:
                case RenderCpuWaitPurpose::Teardown:
                case RenderCpuWaitPurpose::Recovery:
                    return timeout.count() > 0;
                default:
                    return false;
            }
        }
    };
}  // namespace Horo::Render
