#pragma once

/**
 * @file SceneSourceModel.h
 * @brief Backend-neutral authored Scene values shared by bounded source decoding and editor documents.
 *
 * Legacy-only documents retain Scene schema1; typed Character documents require schema2 so older readers reject rather than drop them.
 * Editor visibility and lock fields are retained as inert authoring data and never grant runtime capabilities.
 */

#include "Horo/AI/AISceneComponents.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/ComponentRegistry.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabIdentity.h"
#include "Horo/Prefab/PrefabValueOverrideTypes.h"
#include "Horo/Runtime/Scene/CharacterSceneComponents.h"
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
        std::optional<Runtime::CharacterControllerComponent> characterController;
        std::optional<Runtime::RigidBodyComponent> rigidBody;
        std::vector<Runtime::ColliderComponent> colliders;
        std::vector<Runtime::PhysicsConstraintComponent> physicsConstraints;
        std::vector<Gameplay::BehaviorComponent> behaviors;
        std::vector<Gameplay::SerializedComponent> gameplayComponents;

        [[nodiscard]] bool operator==(const SceneObjectComponentSet &) const noexcept = default;
    };

    /** @brief Durable derivation evidence copied from an explicitly composed generator, never executable authority. */
    struct SceneGenerationSource final {
        Assets::AssetId asset;
        std::uint64_t identity{};
        std::uint64_t revision{};
        std::uint64_t execution{};
        std::uint64_t node{};
        std::uint64_t pin{};
        std::uint64_t sample{};
        std::uint32_t ordinal{};
        [[nodiscard]] auto operator<=>(const SceneGenerationSource &) const noexcept = default;
    };

    /** @brief Target-owned exact generation provenance; names, folders and hierarchy never authorize cleanup.
     * @details Scene stores generic stable producer evidence. Downstream adapters translate their typed identities;
     * Scene never interprets a graph, invokes a generator or selects a provider. Content excludes this ownership tag.
     */
    struct SceneGeneratedOwnership final {
        SceneGenerationSource source;
        std::uint64_t owner{};
        std::uint64_t scope{};
        std::uint64_t lineage{};
        std::uint64_t set{};
        std::uint64_t output{};
        std::uint64_t revision{};
        std::uint64_t ownershipGeneration{};
        Sha256Digest content{};
        [[nodiscard]] auto operator<=>(const SceneGeneratedOwnership &) const noexcept = default;
    };

    /** @brief Check complete inert generation evidence without interpreting or executing its producer.
     * @param value Target-owned provenance. @return Whether every stable identity and content stamp is nonzero.
     */
    [[nodiscard]] inline bool IsValidSceneGeneratedOwnership(const SceneGeneratedOwnership &value) noexcept {
        const auto &source = value.source;
        return source.asset.IsValid() && source.identity && source.revision && source.execution && source.node && source.pin &&
               source.sample && value.owner && value.scope && value.lineage && value.set && value.output && value.revision &&
               value.ownershipGeneration && value.content != Sha256Digest{};
    }

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
        std::optional<SceneGeneratedOwnership> generated; /**< Persisted inert target-owned cleanup and replay evidence. */
    };

    /** @brief Lightweight authored placement of one prefab asset in a containing scene. */
    struct ScenePrefabInstance final {
        Prefab::PrefabInstanceId instanceId;                          /**< Stable scene-local occurrence identity. */
        Prefab::PrefabAssetReference sourcePrefab;                    /**< Path-independent source asset identity. */
        std::optional<SceneObjectId> parent;                          /**< Optional containing-scene parent; never a prefab member. */
        Math::Transform rootTransform;                                /**< Placement transform, separate from prefab-local transforms. */
        std::optional<Prefab::PrefabValueOverrideSet> valueOverrides; /**< Exact source-relative intent; absence preserves legacy bytes. */

        [[nodiscard]] bool operator==(const ScenePrefabInstance &) const noexcept = default;
    };

}  // namespace Horo::SceneSource
