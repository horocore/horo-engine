#include "AndroidWindowBridge.h"

#if defined(__ANDROID__)
#include <android/native_window.h>
#endif

namespace Horo::Platform::Android {
    /** @copydoc NativeAndroidWindowOperations */
    AndroidWindowOperations NativeAndroidWindowOperations() noexcept {
#if defined(__ANDROID__)
        return {.acquire = [](void *, ANativeWindow *window) noexcept {
            ANativeWindow_acquire(window);
            return true;
        }, .release = [](void *, ANativeWindow *window) noexcept {
            ANativeWindow_release(window);
        }};
#else
        return {};
#endif
    }
}  // namespace Horo::Platform::Android
