#pragma once

/**
 * @file SceneIdentity.h
 * @brief Persistent authored Scene identities shared by component schemas and runtime definitions.
 */

#include <compare>
#include <cstddef>
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

    /** @brief Separates child composition policies at the Scene ownership boundary. */
    enum class SceneInstanceMode : std::uint8_t {
        Embedded,  /**< Child entities share the parent's canonical runtime. */
        Streamed,  /**< A streaming authority controls residency relative to the parent. */
        Activated, /**< Independent child runtime linked to its parent's lifetime. */
    };

    struct SceneInstanceIdentityTag;
    /** @brief Stable authored child occurrence identity within one parent. */
    using SceneInstanceId = SceneStableId<SceneInstanceIdentityTag>;

    /** @brief Absolute concurrent Activated child ceiling for one live parent. */
    inline constexpr std::size_t MaximumActivatedSceneInstances = 32;

    /** @brief Monotonic authored content revision carried through runtime activation. */
    struct SceneDefinitionRevision final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const SceneDefinitionRevision &) const noexcept = default;
    };

    /** @brief Unique identity of one activated runtime-scene instance. */
    struct SceneRuntimeId {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SceneRuntimeId &) const noexcept = default;
    };

}  // namespace Horo::Runtime
