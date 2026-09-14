#pragma once

/**
 * @file SceneIdentity.h
 * @brief Persistent authored Scene identities shared by component schemas and runtime definitions.
 */

#include <compare>
#include <cstdint>

namespace Horo::Runtime {
    /** @brief Non-interchangeable stable scene-domain identity with zero reserved as invalid. */
    template <typename Tag> struct SceneStableId final {
        std::uint64_t value{};

        /** @brief Checks representation only. @return Whether the identity is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SceneStableId &) const noexcept = default;
    };

    struct SceneDefinitionIdentityTag;
    struct SceneObjectIdentityTag;

    /** @brief Stable logical identity of one authored scene. */
    using SceneDefinitionId = SceneStableId<SceneDefinitionIdentityTag>;

    /** @brief Stable authored object identity, independent from process-local entity handles. */
    using SceneObjectId = SceneStableId<SceneObjectIdentityTag>;

    /** @brief Monotonic authored content revision carried through runtime activation. */
    struct SceneDefinitionRevision final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const SceneDefinitionRevision &) const noexcept = default;
    };

}  // namespace Horo::Runtime
