#pragma once

/**
 * @file NullProvider.h
 * @brief Explicit feature-absent navigation query provider.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationBackend.h"

#include <memory>

namespace Horo::Navigation {
    /**
     * @brief Creates an inert provider that reports missing navigation data for every grounded query.
     * @return Independently owned provider, or a typed allocation failure.
     */
    [[nodiscard]] Result<std::unique_ptr<INavigationQueryBackend>> CreateNullNavigationQueryBackend();
}  // namespace Horo::Navigation
