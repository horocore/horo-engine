#pragma once

#include "Horo/Network/SceneReplicationCommitSource.h"

#include <exception>

namespace Horo::Network::Detail {
    inline constexpr std::size_t SceneTransformPropertyCount = static_cast<std::size_t>(SceneReplicationProperty::RotationW) + 1;
    static_assert(SceneTransformPropertyCount <= 8);

    /** @brief Finds the bit of a previously validated transform binding. */
    [[nodiscard]] constexpr std::uint8_t TransformPropertyBit(SceneReplicationProperty property) noexcept {
        return static_cast<std::uint8_t>(1U << static_cast<unsigned int>(property));
    }

    inline constexpr auto SceneRotationProperties =
        TransformPropertyBit(SceneReplicationProperty::RotationX) | TransformPropertyBit(SceneReplicationProperty::RotationY) |
        TransformPropertyBit(SceneReplicationProperty::RotationZ) | TransformPropertyBit(SceneReplicationProperty::RotationW);

    /** @brief Allows a translation-only projection or a complete quaternion without mixing rotation generations. */
    [[nodiscard]] constexpr bool HasCompleteRotation(const std::uint8_t properties) noexcept {
        const auto rotation = properties & SceneRotationProperties;
        return rotation == 0 || rotation == SceneRotationProperties;
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
        }
        std::terminate();
    }
}  // namespace Horo::Network::Detail
