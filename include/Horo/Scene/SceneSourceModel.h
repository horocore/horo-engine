#pragma once

/**
 * @file SceneSourceModel.h
 * @brief Backend-neutral authored Scene values shared by bounded source decoding and editor documents.
 *
 * These are the existing Scene schema-1 values, not a new persisted format. Editor visibility
 * and lock fields are retained as inert authoring data and never grant runtime capabilities.
 */

#include "Horo/AI/AISceneComponents.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/PrimitiveMeshDescriptor.h"

#include <optional>
#include <string>
#include <vector>

namespace Horo::SceneSource {
    /** @brief Stable identity of an authored scene object within one document session. */
    struct SceneObjectId {
        std::uint64_t value{0};

        /** @brief Reports whether this ID can identify an object. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SceneObjectId &) const noexcept = default;
    };

    /** @brief Persisted editor-only visibility and interaction state for one authored object. */
    struct SceneObjectEditorState {
        bool visible{true}; /**< Local editor viewport visibility; runtime activation is unaffected. */
        bool locked{false}; /**< Local editor mutation lock; selection remains available for unlocking. */

        [[nodiscard]] constexpr bool operator==(const SceneObjectEditorState &) const noexcept = default;
    };

    /** @brief Typed authored component values attached to one scene object. */
    struct SceneObjectComponentSet {
        std::optional<Runtime::CameraComponent> camera;
        std::optional<Runtime::LightComponent> light;
        std::optional<Runtime::TriggerVolumeComponent> triggerVolume;
        std::optional<Runtime::AudioSourceComponent> audioSource;
        std::optional<Runtime::NavigationSurfaceComponent> navigationSurface;
        std::optional<Runtime::NavigationRegionComponent> navigationRegion;
        std::optional<Runtime::NavigationModifierComponent> navigationModifier;
        std::optional<Runtime::NavigationLinkComponent> navigationLink;
        std::optional<Runtime::NavigationAgentComponent> navigationAgent;
        std::optional<AI::AiAgentComponent> aiAgent;
        std::optional<AI::AiControllerComponent> aiController;
        std::optional<Runtime::RigidBodyComponent> rigidBody;
        std::vector<Runtime::ColliderComponent> colliders;
        std::vector<Runtime::PhysicsConstraintComponent> physicsConstraints;
        std::vector<Gameplay::BehaviorComponent> behaviors;
        std::vector<Gameplay::SerializedComponent> gameplayComponents;

        [[nodiscard]] bool operator==(const SceneObjectComponentSet &) const noexcept = default;
    };

    /** @brief Immutable value snapshot of one authored scene object. */
    struct SceneObjectSnapshot {
        SceneObjectId id;
        std::optional<SceneObjectId> parent;
        std::string name;
        Math::Transform localTransform;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitiveMesh;
        SceneObjectComponentSet components;
        std::optional<Assets::AssetId> meshAsset; /**< Stable imported core.mesh identity. */
        SceneObjectEditorState editorState;
    };

    /** @brief Lightweight authored placement of one prefab asset in a containing scene. */
    struct ScenePrefabInstance final {
        Prefab::PrefabInstanceId instanceId;       /**< Stable scene-local occurrence identity. */
        Prefab::PrefabAssetReference sourcePrefab; /**< Path-independent source asset identity. */
        std::optional<SceneObjectId> parent;       /**< Optional containing-scene parent; never a prefab member. */
        Math::Transform rootTransform;             /**< Placement transform, separate from prefab-local transforms. */

        [[nodiscard]] bool operator==(const ScenePrefabInstance &) const noexcept = default;
    };

}  // namespace Horo::SceneSource
