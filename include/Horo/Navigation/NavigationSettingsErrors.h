#pragma once
/** @file NavigationSettingsErrors.h @brief Asset-free navigation authoring admission vocabulary. */
#include "Horo/Foundation/Result.h"

namespace Horo::Navigation {
    /** @brief Closed settings/workflow failures; lower-layer validation errors remain unchanged. */
    enum class NavigationSettingsFailure : std::uint8_t {
        Invalid,
        UnsupportedVersion,
        Stale,
        Referenced,
        IncompleteInventory,
        Closed,
        Capacity
    };
    /** @brief Returns one stable settings diagnostic. @param failure Exact failure. @return Registered-shape descriptor. */
    [[nodiscard]] const ErrorCodeDescriptor &NavigationSettingsError(NavigationSettingsFailure failure) noexcept;

}  // namespace Horo::Navigation
