#include "Horo/Platform/AccessibilityBridge.h"
#include "Horo/Platform/AccessibilitySettings.h"

int main() {
    Horo::Platform::NullPlatformAccessibilityBridge bridge;
    const auto descriptor = bridge.Descriptor();
    bridge.Shutdown();
    bridge.Shutdown();
    return descriptor.capability == Horo::Platform::AccessibilityBridgeCapability::Unsupported &&
                   descriptor.technology == Horo::Platform::AccessibilityTechnology::None && descriptor.maximumQueuedMessages == 0 &&
                   descriptor.maximumQueuedBytes == 0
               ? 0
               : 1;
}
