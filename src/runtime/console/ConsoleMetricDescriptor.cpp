#include "Horo/Debug/ConsoleBuiltins.h"

namespace Horo::Debug {
    /** @copydoc ConsoleMetricDescriptor */
    DebugCommandDescriptor ConsoleMetricDescriptor() noexcept {
        return {MetricsCommandId,
                "sys.metrics",
                "Inspect authoritative metric availability",
                "sys.metrics",
                {},
                CommandPermission::Developer,
                CommandThreadPolicy::OwnerThreadNextFrame,
                CommandAvailability::NonShipping};
    }
}  // namespace Horo::Debug
