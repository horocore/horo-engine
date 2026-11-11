#include "ConsoleProductInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

namespace Horo::Debug::Detail {
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

    /** @copydoc AppendPublicVariables */
    void AppendPublicVariables(std::vector<ConsoleVariableDescriptor> &descriptors) {
#if !defined(HORO_PROFILE_SERVER)
        ConsoleVariableDescriptor visible{ConsoleVisibleVariableId, "console.visible", false};
        visible.permission = CommandPermission::Public;
        visible.availability = CommandAvailability::ShippingAllowlist;
        descriptors.push_back(std::move(visible));
#else
        (void)descriptors;
#endif
    }
}  // namespace Horo::Debug::Detail
