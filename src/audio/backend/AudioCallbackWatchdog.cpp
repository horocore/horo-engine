#include "Horo/Audio/Internal/AudioCallbackWatchdog.h"

#include "Horo/Audio/Internal/AudioCallbackSafetyHooks.h"

#include <algorithm>

namespace Horo::Audio::Backend {
    /** @copydoc AudioCallbackWatchdog::Configure */
    void AudioCallbackWatchdog::Configure(const std::uint64_t deadlineNanoseconds,
                                          const std::uint64_t minimumFramesBetweenRecords) noexcept {
        deadlineNanoseconds_ = deadlineNanoseconds;
        minimumFramesBetweenRecords_ = minimumFramesBetweenRecords;
        recorded_.fill(false);
        lastEpoch_ = {};
    }

    void AudioCallbackWatchdog::Record(const AudioCallbackViolationKind kind, const std::uint64_t observedNanoseconds) noexcept {
        const auto index = static_cast<std::size_t>(kind);
        if (activeEpoch_ != lastEpoch_) {
            lastEpoch_ = activeEpoch_;
            recorded_.fill(false);
        }
        if (recorded_[index] && activeFrame_ >= lastRecordedFrame_[index] &&
            activeFrame_ - lastRecordedFrame_[index] < minimumFramesBetweenRecords_) {
            rateLimited_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const auto write = write_.load(std::memory_order_relaxed);
        if (write - read_.load(std::memory_order_acquire) == Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        records_[write % Capacity] = {activeEpoch_, activeFrame_, kind, observedNanoseconds, deadlineNanoseconds_};
        lastRecordedFrame_[index] = activeFrame_;
        recorded_[index] = true;
        write_.store(write + 1, std::memory_order_release);
    }

    void AudioCallbackWatchdog::RecordAttempt(void *const context, const Safety::AudioCallbackAttempt attempt) noexcept {
        auto &watchdog = *static_cast<AudioCallbackWatchdog *>(context);
        const auto kind = attempt == Safety::AudioCallbackAttempt::Allocation ? AudioCallbackViolationKind::AllocationAttempt
                                                                              : AudioCallbackViolationKind::LockAttempt;
        watchdog.Record(kind, 0);
    }

    /** @copydoc AudioCallbackWatchdog::ObserveDuration */
    void AudioCallbackWatchdog::ObserveDuration(const AudioDeviceEpoch &epoch, const std::uint64_t sampleFrame,
                                                const std::uint64_t elapsedNanoseconds) noexcept {
#if !defined(NDEBUG)
        activeEpoch_ = epoch;
        activeFrame_ = sampleFrame;
        latestDurationNanoseconds_.store(elapsedNanoseconds, std::memory_order_relaxed);
        sampledCallbacks_.fetch_add(1, std::memory_order_relaxed);
        if (deadlineNanoseconds_ != 0 && elapsedNanoseconds > deadlineNanoseconds_)
            Record(AudioCallbackViolationKind::Deadline, elapsedNanoseconds);
#else
        static_cast<void>(epoch);
        static_cast<void>(sampleFrame);
        static_cast<void>(elapsedNanoseconds);
#endif
    }

    /** @copydoc AudioCallbackWatchdog::Invoke */
    RenderResult AudioCallbackWatchdog::Invoke(const RenderPort &port, const RenderInvocation &invocation) noexcept {
#if defined(NDEBUG)
        return port.process(port.context, invocation);
#else
        activeEpoch_ = invocation.epoch;
        activeFrame_ = invocation.sampleFrame;
        const Safety::AudioCallbackSafetyScope scope(this, &AudioCallbackWatchdog::RecordAttempt);
        const auto start = std::chrono::steady_clock::now();
        const RenderResult result = port.process(port.context, invocation);
        const auto end = std::chrono::steady_clock::now();
        if (const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count(); elapsed > 0)
            ObserveDuration(invocation.epoch, invocation.sampleFrame, static_cast<std::uint64_t>(elapsed));
        return result;
#endif
    }

    /** @copydoc AudioCallbackWatchdog::InvokeWithoutDeadline */
    RenderResult AudioCallbackWatchdog::InvokeWithoutDeadline(const RenderPort &port, const RenderInvocation &invocation) noexcept {
#if defined(NDEBUG)
        return port.process(port.context, invocation);
#else
        activeEpoch_ = invocation.epoch;
        activeFrame_ = invocation.sampleFrame;
        const Safety::AudioCallbackSafetyScope scope(this, &AudioCallbackWatchdog::RecordAttempt);
        const RenderResult result = port.process(port.context, invocation);
        return result;
#endif
    }

    /** @copydoc AudioCallbackWatchdog::OnAllocationAttempt */
    void AudioCallbackWatchdog::OnAllocationAttempt() noexcept {
        Safety::OnAudioAllocationAttempt();
    }

    /** @copydoc AudioCallbackWatchdog::OnLockAttempt */
    void AudioCallbackWatchdog::OnLockAttempt() noexcept {
        Safety::OnAudioLockAttempt();
    }

    /** @copydoc AudioCallbackWatchdog::Drain */
    AudioCallbackViolationDrain AudioCallbackWatchdog::Drain(const std::span<AudioCallbackViolation> output) noexcept {
        auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto count = std::min<std::size_t>(output.size(), write - read);
        for (std::size_t index = 0; index < count; ++index)
            output[index] = records_[(read + static_cast<std::uint32_t>(index)) % Capacity];
        read += static_cast<std::uint32_t>(count);
        read_.store(read, std::memory_order_release);
        return {count, dropped_.exchange(0, std::memory_order_relaxed), rateLimited_.exchange(0, std::memory_order_relaxed),
                sampledCallbacks_.exchange(0, std::memory_order_relaxed), latestDurationNanoseconds_.load(std::memory_order_relaxed)};
    }
}  // namespace Horo::Audio::Backend
