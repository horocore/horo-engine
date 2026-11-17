#pragma once

/** @file AndroidWindowBridge.h
 * @brief Non-installed Android host boundary for generation-correlated native-window retention.
 */
#include "AndroidLifecycleAdapter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

struct ANativeWindow;

namespace Horo::Platform::Android {
    class AndroidWindowBridge;

    /**
     * @brief Move-only platform reference pin for one adapter-issued window generation.
     * Release on the application owner thread, after all native consumers have retired.
     * The bridge and native dispatch context must outlive every lease. A pin is not WSI/GPU completion.
     */
    class AndroidWindowLease final {
    public:
        AndroidWindowLease() noexcept = default;
        AndroidWindowLease(AndroidWindowLease &&other) noexcept;
        AndroidWindowLease &operator=(AndroidWindowLease &&other) noexcept;
        AndroidWindowLease(const AndroidWindowLease &) = delete;
        AndroidWindowLease &operator=(const AndroidWindowLease &) = delete;
        ~AndroidWindowLease();
        /**
         * @brief Borrow the pinned native window; null for an empty/moved lease.
         * @return Native reference valid for retirement after loss, not authorization for new frames.
         * The backend must separately validate current lifecycle/frame admission before new work.
         */
        [[nodiscard]] ANativeWindow *Get() const noexcept;
        /** @brief Release this pin idempotently after actual consumers retire. */
        void Reset() noexcept;

    private:
        AndroidWindowLease(AndroidWindowBridge &owner, std::size_t slot) noexcept;
        AndroidWindowBridge *owner_{};
        std::size_t slot_{};
        friend class AndroidWindowBridge;
    };
    /** @brief Fixed host selection; Vulkan is retention intent, not device or presentation readiness. */
    enum class AndroidWindowSelection : std::uint8_t {
        NoPresentation,
        AndroidVulkan,
        Unsupported
    };

    /**
     * @brief Borrowed native dispatch; context outlives the bridge. Failed acquire owns no reference.
     * Calls cannot throw or release existing leases reentrantly. Other reentrant bridge operations fail explicitly.
     */
    struct AndroidWindowOperations {
        void *context{};
        bool (*acquire)(void *, ANativeWindow *) noexcept {};
        void (*release)(void *, ANativeWindow *) noexcept {};
    };

    /** @brief Real NDK reference operations on Android; absent operations on every other platform. */
    [[nodiscard]] AndroidWindowOperations NativeAndroidWindowOperations() noexcept;

    /**
     * @brief Platform-owned reference admission on the controller's owner thread.
     * The host fixes selection before forwarding native window callbacks. This owner does not
     * create graphics resources or publish PresentationAvailable. Controller and adapter outlive it.
     * All calls and destruction belong to the controller owner thread. Route every native-window
     * observation and final shutdown through this owner; use the adapter for process/Activity observations.
     * Drain through this bridge, not directly through the controller, once native references exist.
     * One attachment pin per generation shares the bounded entry budget with queued candidates.
     */
    class AndroidWindowBridge final {
    public:
        /**
         * @brief Bind the adapter's actual controller, never a second generation allocator.
         * @param adapter Sole generation authority, retained through bridge destruction.
         * @param selection Host-fixed policy selected before native window/graphics use.
         * @param operations Native dispatch whose context outlives this bridge.
         * @pre Construct on the controller's captured owner thread.
         */
        AndroidWindowBridge(AndroidLifecycleAdapter &adapter, AndroidWindowSelection selection,
                            AndroidWindowOperations operations) noexcept;
        AndroidWindowBridge(const AndroidWindowBridge &) = delete;
        AndroidWindowBridge &operator=(const AndroidWindowBridge &) = delete;
        /** @brief Final safety net; callback producers must be detached and all leases released first. */
        ~AndroidWindowBridge();

        /** @brief Retain a callback candidate and enqueue its adapter-issued generation, without committing it. */
        [[nodiscard]] Result<AndroidWindowGeneration> OnWindowAvailable(AndroidActivityGeneration activity, ANativeWindow *window);

        /** @brief Drain the real lifecycle cutoff and reconcile retained references to its committed window. */
        [[nodiscard]] Result<std::size_t> Drain();

        /**
         * @brief Close new pins immediately, then enqueue loss for the exact issued generation.
         * @return Queue result; on saturation the host drains and retries this exact loss.
         * A failed handoff never reopens acquisition or pretends the controller committed loss.
         */
        [[nodiscard]] Result<void> OnWindowLost(AndroidWindowGeneration window);

        /** @brief Pin only the current committed generation; loss/replacement immediately closes new pins. */
        [[nodiscard]] Result<AndroidWindowLease> AcquireWindow(AndroidWindowGeneration window);

        /**
         * @brief Close native admission and drain platform references without blocking.
         * @return True after all pins release, false while consumers still own a pin, or a typed queue/drain error.
         * The host first detaches callback producers, then retires backend users and retries this method.
         * Only the host calls CompleteShutdown after its remaining reverse dependency teardown.
         */
        [[nodiscard]] Result<bool> Shutdown();

    private:
        struct NativeRelease {
            AndroidWindowOperations operations{};

            void operator()(ANativeWindow *window) const noexcept {
                operations.release(operations.context, window);
            }
        };

        using NativeReference = std::unique_ptr<ANativeWindow, NativeRelease>;

        struct Entry {
            NativeReference reference{nullptr, NativeRelease{}};
            AndroidActivityGeneration activity{};
            AndroidWindowGeneration generation{};
            std::size_t pins{};
            bool retired{};
            bool admissionClosed{};
        };

        void ReleasePin(std::size_t slot) noexcept;
        [[nodiscard]] Result<void> CheckAccess() const;
        [[nodiscard]] Result<void> CheckAdmission() const;
        [[nodiscard]] Result<void> CheckWindowRequest(ANativeWindow *window) const;
        [[nodiscard]] Result<NativeReference> Retain(ANativeWindow *window) const;
        [[nodiscard]] Result<std::size_t> DrainLifecycle();
        AndroidLifecycleController &controller_;
        AndroidLifecycleAdapter &adapter_;
        const AndroidWindowSelection selection_;
        const AndroidWindowOperations operations_;
        std::array<Entry, AndroidLifecycleController::MaximumQueueCapacity + 1> entries_{};
        Entry *observed_{};
        const std::thread::id ownerThread_{std::this_thread::get_id()};
        bool operationActive_{}; /**< Owner-thread reentry guard, not cross-thread synchronization. */
        bool closing_{};
        friend class AndroidWindowLease;
    };
}  // namespace Horo::Platform::Android
