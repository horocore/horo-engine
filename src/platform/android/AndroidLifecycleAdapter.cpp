#include "AndroidLifecycleAdapter.h"

#include "Horo/Platform/PlatformErrors.h"

#include <limits>

namespace Horo::Platform::Android {
    namespace {
        [[nodiscard]] Result<std::uint64_t> ReserveGeneration(std::atomic<std::uint64_t> &next) {
            std::uint64_t candidate = next.load();
            while (candidate != 0 && candidate != std::numeric_limits<std::uint64_t>::max()) {
                if (next.compare_exchange_weak(candidate, candidate + 1))
                    return Result<std::uint64_t>::Success(candidate);
            }
            return Result<std::uint64_t>::Failure(MakeError(PlatformErrors::LifecycleGenerationExhausted));
        }
    }  // namespace

    AndroidLifecycleAdapter::AndroidLifecycleAdapter(AndroidLifecycleController &controller) noexcept : controller_(controller) {}

    /** @copydoc AndroidLifecycleAdapter::IsBoundTo */
    bool AndroidLifecycleAdapter::IsBoundTo(const AndroidLifecycleController &controller) const noexcept {
        return &controller_ == &controller;
    }

    Result<void> AndroidLifecycleAdapter::OnProcessCreated() {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ProcessCreated});
    }

    Result<AndroidActivityGeneration> AndroidLifecycleAdapter::OnActivityCreated() {
        auto reserved = ReserveGeneration(nextActivity_);
        if (reserved.HasError())
            return Result<AndroidActivityGeneration>::Failure(reserved.ErrorValue());
        const AndroidActivityGeneration generation{reserved.Value()};
        if (Result<void> queued = controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityCreated, .activity = generation});
            queued.HasError())
            return Result<AndroidActivityGeneration>::Failure(queued.ErrorValue());
        return Result<AndroidActivityGeneration>::Success(generation);
    }

    Result<void> AndroidLifecycleAdapter::OnActivityStarted(const AndroidActivityGeneration activity) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityStarted, .activity = activity});
    }

    Result<void> AndroidLifecycleAdapter::OnActivityResumed(const AndroidActivityGeneration activity) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityResumed, .activity = activity});
    }

    Result<void> AndroidLifecycleAdapter::OnActivityPaused(const AndroidActivityGeneration activity) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityPaused, .activity = activity});
    }

    Result<void> AndroidLifecycleAdapter::OnActivityStopped(const AndroidActivityGeneration activity) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityStopped, .activity = activity});
    }

    Result<void> AndroidLifecycleAdapter::OnActivityDestroyed(const AndroidActivityGeneration activity) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::ActivityDestroyed, .activity = activity});
    }

    Result<AndroidWindowGeneration> AndroidLifecycleAdapter::OnWindowAvailable(const AndroidActivityGeneration activity) {
        auto reserved = ReserveGeneration(nextWindow_);
        if (reserved.HasError())
            return Result<AndroidWindowGeneration>::Failure(reserved.ErrorValue());
        const AndroidWindowGeneration generation{reserved.Value()};
        if (Result<void> queued =
                controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::WindowAvailable, .activity = activity, .window = generation});
            queued.HasError())
            return Result<AndroidWindowGeneration>::Failure(queued.ErrorValue());
        return Result<AndroidWindowGeneration>::Success(generation);
    }

    Result<void> AndroidLifecycleAdapter::OnWindowLost(const AndroidWindowGeneration window) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::WindowLost, .window = window});
    }

    Result<AndroidPresentationGeneration> AndroidLifecycleAdapter::OnPresentationAvailable(const AndroidWindowGeneration window) {
        auto reserved = ReserveGeneration(nextPresentation_);
        if (reserved.HasError())
            return Result<AndroidPresentationGeneration>::Failure(reserved.ErrorValue());
        const AndroidPresentationGeneration generation{reserved.Value()};
        if (Result<void> queued = controller_.Enqueue(
                {.kind = AndroidLifecycleEvent::Kind::PresentationAvailable, .window = window, .presentation = generation});
            queued.HasError())
            return Result<AndroidPresentationGeneration>::Failure(queued.ErrorValue());
        return Result<AndroidPresentationGeneration>::Success(generation);
    }

    Result<void> AndroidLifecycleAdapter::OnPresentationLost(const AndroidPresentationGeneration presentation) {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::PresentationLost, .presentation = presentation});
    }

    Result<void> AndroidLifecycleAdapter::OnFinalShutdown() {
        return controller_.Enqueue({.kind = AndroidLifecycleEvent::Kind::FinalShutdown});
    }
}  // namespace Horo::Platform::Android
