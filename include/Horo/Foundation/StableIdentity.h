#pragma once

/**
 * @file StableIdentity.h
 * @brief Small generation-checked identities shared by lower-level model contracts.
 */

#include <compare>
#include <cstdint>

namespace Horo::Foundation {
    /** @brief Strong identity whose stable value is retained while its generation fences replacement. */
    template <typename Tag> struct StableIdentity final {
        using IdentityTag = Tag;

        std::uint64_t stableValue{}; /**< Durable owner-issued value. */
        std::uint32_t generation{};  /**< Non-zero generation for the current publication. */

        /** @brief Checks the representation without consulting an owner registry. @return True when both fields are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return stableValue != 0 && generation != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const StableIdentity &) const noexcept = default;
    };

    /** @brief Tag for the scene-owned property binding identity shared with cinematic model code. */
    struct PropertyBindingIdentityTag;

    /** @brief Stable generation-checked identity of one registered scene property binding. */
    using PropertyBindingId = StableIdentity<PropertyBindingIdentityTag>;
}  // namespace Horo::Foundation
