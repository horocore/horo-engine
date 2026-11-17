#pragma once

/** @file AudioRenderEnvironment.h
 * @brief Retained-thread floating-point environment for canonical Audio processing.
 */

#include <cfenv>
#include <cstdint>
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
#include <xmmintrin.h>
#endif

namespace Horo::Audio::Backend {
    /** @brief Restore the caller's FP state after round-to-nearest processing with native denormal flushing where supported. */
    class AudioRenderEnvironment final {
    public:
        /** @brief Capture caller state and establish the canonical processing environment without allocation. */
        AudioRenderEnvironment() noexcept {
            std::fegetenv(&environment_);
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
            control_ = _mm_getcsr();
#elif defined(__aarch64__)
            __asm__ volatile("mrs %0, fpcr" : "=r"(control_));
#endif
            std::fesetround(FE_TONEAREST);
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
            _mm_setcsr((_mm_getcsr() | 0x8040u) & ~0x6000u);
#elif defined(__aarch64__)
            const auto processing = (control_ | (std::uint64_t{1} << 24)) & ~(std::uint64_t{3} << 22);
            __asm__ volatile("msr fpcr, %0" : : "r"(processing));
#endif
        }

        /** @brief Return all changed FP state to the calling native/control thread. */
        ~AudioRenderEnvironment() {
            std::fesetenv(&environment_);
#if defined(_M_X64) || defined(__x86_64__) || defined(__SSE__)
            _mm_setcsr(static_cast<unsigned int>(control_));
#elif defined(__aarch64__)
            __asm__ volatile("msr fpcr, %0" : : "r"(control_));
#endif
        }

        AudioRenderEnvironment(const AudioRenderEnvironment &) = delete;
        AudioRenderEnvironment &operator=(const AudioRenderEnvironment &) = delete;

    private:
        std::fenv_t environment_{};
        std::uint64_t control_{};
    };
}  // namespace Horo::Audio::Backend
