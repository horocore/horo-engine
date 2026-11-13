#include "Horo/Debug/ConsoleConfiguration.h"

namespace Horo::Debug {
    /** @copydoc ConsoleLogOverlayVariableDescriptor */
    ConsoleVariableDescriptor ConsoleLogOverlayVariableDescriptor() {
        return {0xDB620003, std::string(ConsoleLogOverlayVariableName), false};
    }

    /** @copydoc ConsoleJobsOverlayVariableDescriptor */
    ConsoleVariableDescriptor ConsoleJobsOverlayVariableDescriptor() {
        return {0xDB620007, std::string(ConsoleJobsOverlayVariableName), false};
    }

    /** @copydoc ConsoleNetworkOverlayVariableDescriptor */
    ConsoleVariableDescriptor ConsoleNetworkOverlayVariableDescriptor() {
        return {0xDB62000B, std::string(ConsoleNetworkOverlayVariableName), false};
    }
}  // namespace Horo::Debug
