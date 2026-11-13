#include "Horo/Debug/ConsoleConfiguration.h"

namespace Horo::Debug {
    /** @copydoc ConsoleSceneOverlayVariableDescriptor */
    ConsoleVariableDescriptor ConsoleSceneOverlayVariableDescriptor() {
        ConsoleVariableDescriptor descriptor{0xDB62000D, std::string(ConsoleSceneOverlayVariableName), false};
        descriptor.availability = CommandAvailability::DevelopmentOnly;
        descriptor.applyPolicy = ConsoleVariableApplyPolicy::NextFrame;
        return descriptor;
    }
}  // namespace Horo::Debug
