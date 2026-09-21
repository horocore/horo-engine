#pragma once

/**
 * @file SceneComponents.h
 * @brief Backend-neutral value types for core authored scene-object components.
 */

#include "Horo/Audio/AudioSoundReference.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Ui/UiDocument.h"

#include <cstdint>

namespace Horo::Runtime {
    /** @brief Collider shapes guaranteed by the core physics-facing primitive contract. */
    enum class ColliderShapeType : std::uint8_t {
        Box,
        Sphere,
        Capsule,
        StaticPlane,
    };

    /** @brief Camera projection authored on a core camera object. */
    enum class CameraProjection : std::uint8_t {
        Perspective,
        Orthographic,
    };

    /** @brief Backend-neutral authored camera values. */
    struct CameraComponent {
        CameraProjection projection{CameraProjection::Perspective};
        float verticalFieldOfViewRadians{1.0471976F};
        float orthographicHeight{10.0F};
        float nearPlane{0.1F};
        float farPlane{1000.0F};
        bool enabled{true}; /**< Whether runtime camera behavior is active. */

        [[nodiscard]] constexpr auto operator<=>(const CameraComponent &) const noexcept = default;
    };

    /** @brief Core light kinds available without a package. */
    enum class LightKind : std::uint8_t {
        Directional,
        Point,
        Spot,
    };

    /** @brief Backend-neutral authored light values. */
    struct LightComponent {
        LightKind kind{LightKind::Directional};
        Math::Vec3 color{1.0F, 1.0F, 1.0F};
        float intensity{1.0F};
        float range{10.0F};
        float innerConeRadians{0.3490659F};
        float outerConeRadians{0.7853982F};
        bool enabled{true}; /**< Whether runtime lighting behavior is active. */

        [[nodiscard]] constexpr auto operator<=>(const LightComponent &) const noexcept = default;
    };

    /**
     * @brief Legacy authored overlap-volume convenience component.
     *
     * Scene-document conversion normalizes enabled values into an explicit static
     * sensor body and collider before producing a RuntimeSceneDefinition. This
     * authoring-only value is not part of the runtime component payload.
     */
    struct TriggerVolumeComponent {
        ColliderShapeType shape{ColliderShapeType::Box};
        bool enabled{true}; /**< Whether runtime overlap behavior is active. */

        [[nodiscard]] constexpr auto operator<=>(const TriggerVolumeComponent &) const noexcept = default;
    };

    /**
     * @brief Backend-neutral authored audio emitter.
     *
     * The reference is persistent Audio-owned data; the playback defaults contain no runtime voice or backend
     * state. An unassigned reference is allowed while an editor creates the component.
     */
    struct AudioSourceComponent {
        Audio::AudioSoundReference sound;
        Audio::AudioSoundPlaybackDefaults playback;
        Audio::AudioSceneLifecyclePolicy sceneLifecycle{Audio::AudioSceneLifecyclePolicy::StopOnUnload};
        bool enabled{true}; /**< Whether runtime audio emission is active. */

        [[nodiscard]] constexpr auto operator<=>(const AudioSourceComponent &) const noexcept = default;
    };

    /** @brief Authored scene component that instantiates one canvas asset without owning Runtime UI lifetime. */
    struct UiCanvasComponent final {
        Ui::UiCanvasAssetReference canvas; /**< Stable canvas asset reference resolved by the Runtime UI owner. */
        /** @brief Compares serialized component evidence. @return Structural ordering and equality. */
        [[nodiscard]] constexpr auto operator<=>(const UiCanvasComponent &) const noexcept = default;
    };
}  // namespace Horo::Runtime
