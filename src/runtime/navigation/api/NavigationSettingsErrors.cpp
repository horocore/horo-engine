#include "Horo/Navigation/NavigationSettingsErrors.h"

#include <array>

namespace Horo::Navigation {
    /** @copydoc NavigationSettingsError */
    const ErrorCodeDescriptor &NavigationSettingsError(const NavigationSettingsFailure failure) noexcept {
        using enum ErrorSeverity;
        static const ErrorDomainId domain{"horo.navigation.settings"};
        static const std::array<ErrorCodeDescriptor, 7> errors{
            {{domain, ErrorCode{"navigation_settings.invalid"}, Error, "Navigation settings are invalid.",
              "Correct the complete project policy.", false, true},
             {domain, ErrorCode{"navigation_settings.version"}, Error, "Navigation settings version is unsupported.",
              "Migrate with a compatible editor.", false, true},
             {domain, ErrorCode{"navigation_settings.stale"}, Error, "Navigation settings or references changed.",
              "Capture a fresh preview before applying.", true, true},
             {domain, ErrorCode{"navigation_settings.referenced"}, Error, "Navigation identity is still referenced.",
              "Review every occurrence and choose an explicit surviving replacement.", false, true},
             {domain, ErrorCode{"navigation_settings.inventory_incomplete"}, Error, "Project reference inventory is incomplete.",
              "Load every registered Scene and definition before destructive edits.", false, true},
             {domain, ErrorCode{"navigation_settings.closed"}, Error, "Navigation document is closed or on another thread.",
              "Use the live owner-thread session.", false, true},
             {domain, ErrorCode{"navigation_settings.capacity"}, Error, "Navigation edit exceeds its bounded capacity.",
              "Reduce the transaction or history size.", false, true}}};
        const auto index = static_cast<std::size_t>(failure);
        return errors[index < errors.size() ? index : 0];
    }

}  // namespace Horo::Navigation
