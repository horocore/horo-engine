#pragma once

#include "Horo/Extensions/EditorThemeTokens.h"

namespace Horo::Extensions::ThemeTokenInternal {
    template <typename Role, typename BitFunction>
    [[nodiscard]] inline bool IsSupported(const std::uint64_t mask, const Role role, const BitFunction bitFunction) noexcept {
        const std::uint64_t bit = bitFunction(role);
        return bit != 0U && (mask & bit) != 0U;
    }

    template <typename Role> [[nodiscard]] inline bool IsKnownRole(const Role role, const Role countRole) noexcept {
        return static_cast<std::uint8_t>(role) < static_cast<std::uint8_t>(countRole);
    }

    template <typename Role, typename BitFunction>
    [[nodiscard]] inline bool IsKnownMask(const std::uint64_t mask, const Role countRole, const BitFunction) noexcept {
        const std::uint64_t known = ThemeTokenDetail::AllRoleMask(static_cast<std::uint8_t>(countRole));
        return (mask & ~known) == 0U;
    }
}  // namespace Horo::Extensions::ThemeTokenInternal
