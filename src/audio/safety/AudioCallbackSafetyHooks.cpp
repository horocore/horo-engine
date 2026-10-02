#include "Horo/Audio/Internal/AudioCallbackSafetyHooks.h"

namespace Horo::Audio::Safety {
    namespace {
#if !defined(NDEBUG)
        /** @brief The active callback receiver belongs to this thread; a nested scope saves and restores it. */
        struct CallbackBinding final {
            void *context{};
            AudioCallbackAttemptSink sink{};
        };

        constinit thread_local CallbackBinding activeBinding{};
#endif

        /** @brief Invoke only the already-installed fixed receiver, with no fallback work. */
        void Report(const AudioCallbackAttempt attempt) noexcept {
#if !defined(NDEBUG)
            if (activeBinding.sink)
                activeBinding.sink(activeBinding.context, attempt);
#else
            static_cast<void>(attempt);
#endif
        }
    }  // namespace

    /** @copydoc AudioCallbackSafetyScope::AudioCallbackSafetyScope */
    AudioCallbackSafetyScope::AudioCallbackSafetyScope(void *const context, const AudioCallbackAttemptSink sink) noexcept {
#if !defined(NDEBUG)
        previousContext_ = activeBinding.context;
        previousSink_ = activeBinding.sink;
        activeBinding = {context, sink};
#else
        static_cast<void>(context);
        static_cast<void>(sink);
#endif
    }

    /** @copydoc AudioCallbackSafetyScope::~AudioCallbackSafetyScope */
    AudioCallbackSafetyScope::~AudioCallbackSafetyScope() noexcept {
#if !defined(NDEBUG)
        activeBinding = {previousContext_, previousSink_};
#endif
    }

    /** @copydoc OnAudioAllocationAttempt */
    void OnAudioAllocationAttempt() noexcept {
        Report(AudioCallbackAttempt::Allocation);
    }

    /** @copydoc OnAudioLockAttempt */
    void OnAudioLockAttempt() noexcept {
        Report(AudioCallbackAttempt::Lock);
    }
}  // namespace Horo::Audio::Safety
