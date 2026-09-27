#pragma once

/** @file AudioCallbackSafetyHooks.h
 * @brief Build-tree-only callback scope and explicit Horo audio forbidden-operation hooks.
 */

#include <cstdint>

namespace Horo::Audio::Safety {
    /** @brief Operation attempted by a participating Horo audio API while a render callback is active. */
    enum class AudioCallbackAttempt : std::uint8_t {
        Allocation,
        Lock
    };

    /** @brief Noexcept, non-owning receiver installed only for the duration of one render invocation. */
    using AudioCallbackAttemptSink = void (*)(void *, AudioCallbackAttempt) noexcept;

    /**
     * @brief Restore the previous thread-local callback receiver after a possibly nested invocation.
     *
     * Construction and destruction do not allocate, wait, or call the sink. The owner of context
     * must outlive the invocation; only the callback thread installs a scope.
     */
    class AudioCallbackSafetyScope final {
    public:
        /** @brief Install one receiver for this thread until destruction. */
        AudioCallbackSafetyScope(void *context, AudioCallbackAttemptSink sink) noexcept;
        /** @brief Restore the previous receiver. */
        ~AudioCallbackSafetyScope() noexcept;

        AudioCallbackSafetyScope(const AudioCallbackSafetyScope &) = delete;
        AudioCallbackSafetyScope &operator=(const AudioCallbackSafetyScope &) = delete;

    private:
        void *previousContext_{};
        AudioCallbackAttemptSink previousSink_{};
    };

    /** @brief Report an attempted general-heap allocation at a participating Horo audio site. */
    void OnAudioAllocationAttempt() noexcept;
    /** @brief Report an attempted mutex acquisition at a participating Horo audio site. */
    void OnAudioLockAttempt() noexcept;
}  // namespace Horo::Audio::Safety
