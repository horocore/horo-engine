#pragma once

#include "Horo/Network/SceneReplicationCommitSource.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <exception>

namespace Horo::Network::Detail {
    inline constexpr std::size_t SceneTransformPropertyCount = static_cast<std::size_t>(SceneReplicationProperty::AnimationActionTag) + 1;
    using SceneTransformProperties = std::bitset<SceneTransformPropertyCount>;

    /** @brief Finds the set entry for a previously validated transform binding. */
    [[nodiscard]] constexpr std::size_t TransformPropertyIndex(SceneReplicationProperty property) noexcept {
        return static_cast<std::size_t>(property);
    }

    inline constexpr std::array SceneRotationProperties{SceneReplicationProperty::RotationX, SceneReplicationProperty::RotationY,
                                                        SceneReplicationProperty::RotationZ, SceneReplicationProperty::RotationW};

    /** @brief Distinguishes typed presentation velocity from writable transform properties. */
    [[nodiscard]] constexpr bool IsVelocityProperty(SceneReplicationProperty property) noexcept {
        return property >= SceneReplicationProperty::LinearVelocityX && property <= SceneReplicationProperty::LinearVelocityZ;
    }

    /** @brief Selects one validated axis from world-space motion. */
    [[nodiscard]] inline float VelocityScalar(const Math::Vec3 &velocity, SceneReplicationProperty property) noexcept {
        using enum SceneReplicationProperty;
        switch (property) {
            case LinearVelocityX:
                return velocity.x;
            case LinearVelocityY:
                return velocity.y;
            case LinearVelocityZ:
                return velocity.z;
            default:
                std::terminate();
        }
    }

    /** @brief Counts the quaternion components selected in this projection. */
    [[nodiscard]] inline std::size_t RotationPropertyCount(const SceneTransformProperties &properties) noexcept {
        return static_cast<std::size_t>(std::ranges::count_if(SceneRotationProperties, [&](SceneReplicationProperty property) {
            return properties.test(TransformPropertyIndex(property));
        }));
    }

    /** @brief Allows a translation-only projection or a complete quaternion without mixing rotation generations. */
    [[nodiscard]] constexpr bool HasCompleteRotation(const std::size_t count) noexcept {
        return count == 0 || count == SceneRotationProperties.size();
    }

    /** @brief Selects the same validated transform member for capture and apply. */
    [[nodiscard]] inline float &TransformScalar(Math::Transform &transform, SceneReplicationProperty property) noexcept {
        using enum SceneReplicationProperty;
        switch (property) {
            case TranslationX:
                return transform.translation.x;
            case TranslationY:
                return transform.translation.y;
            case TranslationZ:
                return transform.translation.z;
            case RotationX:
                return transform.rotation.x;
            case RotationY:
                return transform.rotation.y;
            case RotationZ:
                return transform.rotation.z;
            case RotationW:
                return transform.rotation.w;
            default:
                std::terminate();
        }
    }
}  // namespace Horo::Network::Detail
