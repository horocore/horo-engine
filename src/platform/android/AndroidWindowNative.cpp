#include "AndroidWindowHost.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace Horo::Platform::Android {
    namespace {
#if defined(__ANDROID__)
        /** @brief Retains the actual callback-borrowed NDK window before GameActivity releases its reference. */
        void AcquireWindow(void *, void *window) noexcept {
            ANativeWindow_acquire(static_cast<ANativeWindow *>(window));
        }

        /** @brief Releases only a platform-owned NDK reference after its exact renderer leases retire. */
        void ReleaseWindow(void *, void *window) noexcept {
            ANativeWindow_release(static_cast<ANativeWindow *>(window));
        }

        /** @brief Reads actual native pixel bounds; zero/negative native extents remain explicitly nondrawable. */
        AndroidWindowExtent WindowExtent(void *, void *window) noexcept {
            const auto width = ANativeWindow_getWidth(static_cast<ANativeWindow *>(window));
            const auto height = ANativeWindow_getHeight(static_cast<ANativeWindow *>(window));
            return {width > 0 ? static_cast<std::uint32_t>(width) : 0U, height > 0 ? static_cast<std::uint32_t>(height) : 0U};
        }
#endif
    }  // namespace

    /** @brief Selects actual NDK operations only for an Android build; no host simulation is selected by default. */
    AndroidWindowNativeOperations NativeAndroidWindowOperations() noexcept {
#if defined(__ANDROID__)
        return {nullptr, &AcquireWindow, &ReleaseWindow, &WindowExtent};
#else
        return {};
#endif
    }
}  // namespace Horo::Platform::Android
