#pragma once

/** @file AudioCallbackWatchdog.h
 * @brief Build-tree-only, bounded development instrumentation for an audio render callback.
 */

#include "Horo/Audio/Internal/AudioBackend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace Horo::Audio::Safety {
    enum class AudioCallbackAttempt : std::uint8_t;
}

namespace Horo::Audio::Backend {
    /** @brief Fixed callback safety facts; no strings or diagnostic formatting enter the render thread. */
    enum class AudioCallbackViolationKind : std::uint8_t {
        Deadline,
        AllocationAttempt,
        LockAttempt
    };

    /** @brief Copyable callback fact, correlated to the exact callback epoch by control. */
    struct AudioCallbackViolation final {
        AudioDeviceEpoch epoch;
        std::uint64_t sampleFrame{};
        AudioCallbackViolationKind kind{AudioCallbackViolationKind::Deadline};
        std::uint64_t observedNanoseconds{}; /**< Duration for deadline records; zero for hooks. */
        std::uint64_t budgetNanoseconds{};
    };

    /** @brief Bounded control-side drain result; dropped and rate-limited facts remain observable. */
    struct AudioCallbackViolationDrain final {
        std::size_t count{};
        std::uint64_t dropped{};
        std::uint64_t rateLimited{};
        std::uint64_t sampledCallbacks{};          /**< Durations measured since the previous drain. */
        std::uint64_t latestDurationNanoseconds{}; /**< Latest observed duration; summary, not an event correlation. */
    };

    /**
     * @brief Single-callback-producer watchdog with one non-callback control consumer.
     *
     * Configure only before callback attachment or after native detachment. Invoke and explicit
     * allocation/lock hooks do no heap allocation, waiting, formatting or consumer calls. The
     * fixed ring drops new records on saturation; counters retain that loss. Drain must never run
     * on the callback. Hooks observe participating Horo call sites, not arbitrary third-party
     * malloc/new or OS mutex internals. The owner retains this object through callback detachment.
     */
    class AudioCallbackWatchdog final {
    public:
        static constexpr std::size_t Capacity = 64;

        /** @brief Set the per-render deadline and per-kind record interval before native attachment without discarding undrained facts. */
        void Configure(std::uint64_t deadlineNanoseconds, std::uint64_t minimumFramesBetweenRecords) noexcept;

        /** @brief Invoke the retained render port under a bounded development safety scope. */
        [[nodiscard]] RenderResult Invoke(const RenderPort &port, const RenderInvocation &invocation) noexcept;

        /** @brief Invoke a deterministic Null callback with hooks but without reading wall time. */
        [[nodiscard]] RenderResult InvokeWithoutDeadline(const RenderPort &port, const RenderInvocation &invocation) noexcept;

        /** @brief Admit one already-measured callback duration without consulting a clock. Callback producer thread only. */
        void ObserveDuration(const AudioDeviceEpoch &epoch, std::uint64_t sampleFrame, std::uint64_t elapsedNanoseconds) noexcept;

        /** @brief Mark an attempted general-heap allocation at an instrumented call site. */
        static void OnAllocationAttempt() noexcept;
        /** @brief Mark an attempted potentially blocking lock at an instrumented call site. */
        static void OnLockAttempt() noexcept;

        /** @brief Copy a bounded prefix and exchange loss counters on the control thread only. */
        [[nodiscard]] AudioCallbackViolationDrain Drain(std::span<AudioCallbackViolation> output) noexcept;

    private:
        static void RecordAttempt(void *context, Safety::AudioCallbackAttempt attempt) noexcept;
        void Record(AudioCallbackViolationKind kind, std::uint64_t observedNanoseconds) noexcept;

        static constexpr std::size_t KindCount = 3;
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
        std::array<AudioCallbackViolation, Capacity> records_{};
        // Single-producer/single-consumer indices publish a record with release/acquire and
        // release a consumed slot in the opposite direction. Loss/sample counters are independent
        // summaries, so relaxed ordering cannot expose an unpublished record or overwrite a live slot.
        std::atomic<std::uint32_t> write_{};
        std::atomic<std::uint32_t> read_{};
        std::atomic<std::uint64_t> dropped_{};
        std::atomic<std::uint64_t> rateLimited_{};
        std::atomic<std::uint64_t> sampledCallbacks_{};
        std::atomic<std::uint64_t> latestDurationNanoseconds_{};
        std::array<std::uint64_t, KindCount> lastRecordedFrame_{}; /**< Callback-thread only. */
        std::array<bool, KindCount> recorded_{};                   /**< Callback-thread only. */
        AudioDeviceEpoch lastEpoch_{};                             /**< Callback-thread only. */
        AudioDeviceEpoch activeEpoch_{};                           /**< Callback-thread only. */
        std::uint64_t activeFrame_{};                              /**< Callback-thread only. */
        std::uint64_t deadlineNanoseconds_{};                      /**< Fixed before attachment. */
        std::uint64_t minimumFramesBetweenRecords_{};              /**< Fixed before attachment. */
    };
}  // namespace Horo::Audio::Backend
