#include "Horo/Platform/AccessibilityBridge.h"

namespace Horo::Platform {
    /** @copydoc NullPlatformAccessibilityBridge::Descriptor */
    AccessibilityBridgeDescriptor NullPlatformAccessibilityBridge::Descriptor() const noexcept {
        return {};
    }

    /** @copydoc NullPlatformAccessibilityBridge::Submit */
    AccessibilityEnqueueResult NullPlatformAccessibilityBridge::Submit(const Runtime::Ui::UiAccessibilitySnapshot &,
                                                                       const Runtime::Ui::UiAccessibilityChangePublisher &,
                                                                       const Runtime::Ui::UiPresentationReceipt &) noexcept {
        return stopped_ ? AccessibilityEnqueueResult::ShuttingDown : AccessibilityEnqueueResult::Unsupported;
    }

    /** @copydoc NullPlatformAccessibilityBridge::TryDequeueAction */
    bool NullPlatformAccessibilityBridge::TryDequeueAction(Runtime::Ui::UiAccessibilityActionRequest &) noexcept {
        return false;
    }

    /** @copydoc NullPlatformAccessibilityBridge::Shutdown */
    void NullPlatformAccessibilityBridge::Shutdown() noexcept {
        stopped_ = true;
    }
}  // namespace Horo::Platform
