#pragma once

/** @file NavigationLinkTypes.h
 * @brief Shared authored grounded traversal kind and ordered direction vocabulary.
 */

#include <cstdint>

namespace Horo::Navigation {
    /** @brief Closed traversal semantics for one explicit grounded transition. */
    enum class NavigationLinkKind : std::uint8_t {
        Jump,
        Ladder,
        Door,
        Teleport,
        Count,
    };

    /** @brief Direction relative to explicitly named start and end endpoints. */
    enum class NavigationLinkDirection : std::uint8_t {
        StartToEnd,
        Bidirectional,
        Count,
    };
}  // namespace Horo::Navigation
