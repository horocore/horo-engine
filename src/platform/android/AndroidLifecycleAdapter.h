#pragma once

#include "Horo/Platform/AndroidLifecycle.h"

#include <atomic>

namespace Horo::Platform::Android {
    /** @brief Target-private callback adapter; concrete GameActivity glue supplies only callback timing and native ownership. */
    class AndroidLifecycleAdapter final {
    public:
        explicit AndroidLifecycleAdapter(AndroidLifecycleController &controller) noexcept;

        /** @brief Verifies the exact existing lifecycle authority without exposing mutable state. @param controller Expected authority.
         * @return True only when this adapter issues observations for that controller. */
        [[nodiscard]] bool IsBoundTo(const AndroidLifecycleController &controller) const noexcept;

        [[nodiscard]] Result<void> OnProcessCreated();
        [[nodiscard]] Result<AndroidActivityGeneration> OnActivityCreated();
        [[nodiscard]] Result<void> OnActivityStarted(AndroidActivityGeneration activity);
        [[nodiscard]] Result<void> OnActivityResumed(AndroidActivityGeneration activity);
        [[nodiscard]] Result<void> OnActivityPaused(AndroidActivityGeneration activity);
        [[nodiscard]] Result<void> OnActivityStopped(AndroidActivityGeneration activity);
        [[nodiscard]] Result<void> OnActivityDestroyed(AndroidActivityGeneration activity);
        [[nodiscard]] Result<AndroidWindowGeneration> OnWindowAvailable(AndroidActivityGeneration activity);
        [[nodiscard]] Result<void> OnWindowLost(AndroidWindowGeneration window);
        [[nodiscard]] Result<AndroidPresentationGeneration> OnPresentationAvailable(AndroidWindowGeneration window);
        [[nodiscard]] Result<void> OnPresentationLost(AndroidPresentationGeneration presentation);
        [[nodiscard]] Result<void> OnFinalShutdown();

    private:
        AndroidLifecycleController &controller_;
        std::atomic<std::uint64_t> nextActivity_{1};
        std::atomic<std::uint64_t> nextWindow_{1};
        std::atomic<std::uint64_t> nextPresentation_{1};
    };
}  // namespace Horo::Platform::Android
