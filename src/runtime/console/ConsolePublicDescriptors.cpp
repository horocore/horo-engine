#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

namespace Horo::Debug::Detail {
#if !defined(HORO_PROFILE_SERVER)
    /** @copydoc ScreenshotDescriptor */
    DebugCommandDescriptor ScreenshotDescriptor() noexcept {
        return {ScreenshotCommandId,
                "screenshot",
                "Capture a frame through the host's approved capture capability",
                "screenshot",
                {},
                CommandPermission::Public,
                CommandThreadPolicy::RenderSafePoint,
                CommandAvailability::ShippingAllowlist};
    }
#endif

    /** @copydoc AppendPublicVariables */
    void AppendPublicVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
#if !defined(HORO_PROFILE_SERVER)
        descriptors.push_back(ConsoleVisibilityVariableDescriptor());
#else
        (void)descriptors;
#endif
    }
}  // namespace Horo::Debug::Detail
