#pragma once

/**
 * @file AssetDependency.h
 * @brief Backend-neutral typed asset requirement shared by cook and runtime definitions.
 */

#include "Horo/Assets/AssetId.h"

#include <compare>

namespace Horo::Assets {
    /** @brief One required cooked asset and its expected runtime type. */
    struct AssetDependency final {
        AssetId id;               /**< Stable cooked asset identity. */
        AssetTypeId expectedType; /**< Type required by the consuming definition. */
        [[nodiscard]] auto operator<=>(const AssetDependency &) const noexcept = default;
    };
}  // namespace Horo::Assets
