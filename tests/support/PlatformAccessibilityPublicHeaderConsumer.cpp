#include "Horo/Platform/AccessibilityBridge.h"
#include "Horo/Platform/AccessibilitySettings.h"
#include "Horo/Runtime/Ui/UiAccessibilityChanges.h"

int main() {
    Horo::Platform::NullPlatformAccessibilityBridge bridge;
    Horo::Platform::IPlatformAccessibilityBridge &transport = bridge;
    Horo::Runtime::Ui::UiAccessibilityAnnouncementCursor cursor{};
    cursor.sequence = 42;
    const bool completed = transport.TryDequeueAnnouncementCompletion(cursor);
    const auto descriptor = bridge.Descriptor();
    bridge.Shutdown();
    bridge.Shutdown();
    return !completed && cursor.sequence == 42 && descriptor.capability == Horo::Platform::AccessibilityBridgeCapability::Unsupported &&
                   descriptor.technology == Horo::Platform::AccessibilityTechnology::None && descriptor.maximumQueuedMessages == 0 &&
                   descriptor.maximumQueuedBytes == 0
               ? 0
               : 1;
}
