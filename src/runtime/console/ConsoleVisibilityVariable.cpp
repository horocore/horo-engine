#include "Horo/Debug/ConsoleConfiguration.h"

namespace Horo::Debug {
    /** @copydoc ConsoleVisibilityVariableDescriptor */
    ConsoleVariableDescriptor ConsoleVisibilityVariableDescriptor() {
        ConsoleVariableDescriptor visible{ConsoleVisibleVariableId, std::string(ConsoleVisibleVariableName), false};
        visible.permission = CommandPermission::Public;
        visible.availability = CommandAvailability::ShippingAllowlist;
        return visible;
    }
}  // namespace Horo::Debug
