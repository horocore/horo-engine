#pragma once

/** @file FramePacing.h
 * @brief Host-owned bounded frame limiter and truthful presentation timing observations.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Foundation/Time.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycle.h"

#include <array>
#include <optional>
#include <thread>

namespace Horo::Render {
    /** @brief Native display evidence, translated to the host monotonic clock without waiting.
     * Display ordinal counts actual refresh intervals, including intervals without a new frame.
     * CPU present return and GPU completion are never valid producers of this value.
     */
    struct NativePresentTiming final {
        RenderSurfaceId surface;
        std::uint64_t frameNumber{};           /**< Exact successfully submitted host frame. */
        std::uint64_t displayOrdinal{};        /**< Increasing native refresh ordinal; zero when unavailable. */
        Duration displayTime;                  /**< Qualified native display timestamp in the host clock domain. */
        Duration clockUncertainty;             /**< Maximum calibration half-span; bounds timestamp comparison to observation time. */
        std::uint64_t discardedObservations{}; /**< Native provider coalescing, invalid and stale feedback count. */
    };

    /** @brief Synchronously borrowed host identity and clock for one native feedback registration.
     * Backends may sample clock only during PresentWithTiming; callbacks must retain values,
     * never this request or its clock. Clock must advance in monotonic real nanoseconds.
     */
    struct PresentationTimingRequest final {
        RenderSurfaceId surface;
        std::uint64_t frameNumber{};
        Clock &clock;
    };

    /** @brief Independent host rate cap; zero means unlimited and never changes PresentMode. */
    struct FramePacingPolicy final {
        std::uint32_t maximumFramesPerSecond{}; /**< Zero or 1..1000; no implicit refresh-rate cap. */
    };

    /** @brief Whether the host may run a frame or must service events and poll again. */
    enum class FramePacingDisposition : std::uint8_t {
        Ready,
        Wait,
        Suspended
    };

    /** @brief One bounded host scheduling decision; never a GPU wait or simulation delta. */
    struct FramePacingDecision final {
        FramePacingDisposition disposition{FramePacingDisposition::Ready};
        Duration wait; /**< At most 2 ms; host services events and cancellation between polls. */
    };

    /** @brief Allocation-free immutable pacing diagnostics; absent refresh means unqualified. */
    struct FramePacingStatistics final {
        std::uint64_t presentedFrames{};
        std::uint64_t failedPresents{};
        std::uint64_t missedDeadlines{};
        Duration lastPresentCallDuration; /**< CPU call duration, not scanout or GPU latency. */
        std::uint64_t discardedNativeObservations{};
        std::optional<Duration> lastNativeDisplayTime; /**< Actual native display feedback, never CPU present return. */
        std::optional<double> estimatedRefreshHertz;   /**< Native-only rolling estimate after four intervals. */
        std::uint32_t nativeIntervalSamples{};         /**< Bounded to eight intervals of this surface generation. */
    };

    /** @brief Single owner-thread host pacing state; no jobs, callbacks, native handles or sleeping.
     * The host owns clocks/waits and calls Stop before destroying the surface. Configure resets
     * deadlines and native estimates across output revisions and policy changes. Simulation
     * retains its clock. Cancellation is checked on every poll, including unlimited mode.
     */
    class FramePacer final {
    public:
        /** @brief Captures the calling host thread as sole owner. */
        FramePacer() noexcept;
        FramePacer(const FramePacer &) = delete;
        FramePacer &operator=(const FramePacer &) = delete;
        FramePacer(FramePacer &&) = delete;
        FramePacer &operator=(FramePacer &&) = delete;

        /** @brief Publishes an exact surface snapshot and independent cap.
         * @param policy Host policy; unknown/out-of-range values fail without mutation.
         * @param surface Committed lifecycle publication, never a pending output candidate.
         * @return Success or typed policy, surface, stale, thread or stopped failure.
         */
        [[nodiscard]] Result<void> Configure(FramePacingPolicy policy, const RenderSurfaceSnapshot &surface);
        /** @brief Polls admission without blocking; Ready commits the next cap deadline.
         * @param now Non-negative host monotonic time in nanoseconds.
         * @param cancelled Current host cancellation state, sampled again after every wait slice.
         * @return Bounded scheduling decision or typed clock/cancellation/lifecycle failure.
         */
        [[nodiscard]] Result<FramePacingDecision> Poll(Duration now, bool cancelled);
        /** @brief Samples the real host cancellation token at each bounded admission poll.
         * @param now Current host monotonic time.
         * @param cancellation Host-owned startup token, borrowed only for this poll.
         * @param closing Additional owner-thread close request.
         * @return Same scheduling decision or typed cancellation/lifecycle failure as Poll.
         */
        [[nodiscard]] Result<FramePacingDecision> Poll(Duration now, const CancellationToken &cancellation, bool closing = false);
        /** @brief Records CPU present outcome and optional separately qualified display evidence.
         * @param frameNumber Increasing host frame ordinal.
         * @param start Host clock immediately before Present.
         * @param end Host clock immediately after Present.
         * @param succeeded Whether Present succeeded; failure never contributes refresh evidence.
         * @param native Optional backend display evidence, possibly delayed within this generation.
         * @param observedAt Host time after polling feedback; defaults to end when absent.
         * @return Success or typed clock, identity, stale, native-evidence or lifecycle failure.
         */
        [[nodiscard]] Result<void> RecordPresent(std::uint64_t frameNumber, Duration start, Duration end, bool succeeded,
                                                 const std::optional<NativePresentTiming> &native = std::nullopt,
                                                 std::optional<Duration> observedAt = std::nullopt);
        /** @brief Clears timing baselines after a host focus change without changing policy.
         * @return Success or typed owner/stopped failure.
         */
        [[nodiscard]] Result<void> Reset();
        /** @brief Returns owned diagnostics on the host thread. @return Snapshot or typed thread failure. */
        [[nodiscard]] Result<FramePacingStatistics> Statistics() const;
        /** @brief Idempotently closes admission and drops retained timing evidence.
         * @return Success or typed thread failure. No waits or callbacks occur.
         */
        [[nodiscard]] Result<void> Stop();

    private:
        /** @brief Checks affinity before reading or changing host-owned state. @return Success or typed thread failure. */
        [[nodiscard]] Result<void> CheckOwner() const;
        /** @brief Drops deadlines and native estimates without changing cumulative diagnostics. */
        void ClearBaselines() noexcept;
        /** @brief Adds one qualified interval to bounded native history. @param interval Valid 1 ms..1 s refresh period. */
        void AppendInterval(std::int64_t interval) noexcept;
        /** @brief Validates admission before reading deadline state. @param stamp Host monotonic nanoseconds.
         * @param cancelled Sampled host cancellation. @return Success or typed owner/lifecycle/clock failure.
         */
        [[nodiscard]] Result<void> ValidatePoll(std::int64_t stamp, bool cancelled) const;
        /** @brief Validates one CPU receipt without committing timing history.
         * @param frame Host ordinal. @param start Present entry. @param end Present return.
         * @param observed Time after native feedback polling. @return Success or typed admission failure.
         */
        [[nodiscard]] Result<void> ValidatePresent(std::uint64_t frame, Duration start, Duration end, Duration observed) const;
        std::thread::id ownerThread_;
        bool stopped_{};
        std::optional<RenderSurfaceSnapshot> surface_;
        FramePacingPolicy policy_;
        FramePacingStatistics statistics_;
        std::optional<std::int64_t> deadline_;
        std::optional<std::int64_t> lastPoll_;
        std::optional<Duration> lastPresentEnd_;
        std::uint64_t lastFrame_{};
        std::optional<NativePresentTiming> lastNative_;
        std::array<std::int64_t, 8> intervals_{};
        std::size_t intervalCursor_{};
    };
}  // namespace Horo::Render
