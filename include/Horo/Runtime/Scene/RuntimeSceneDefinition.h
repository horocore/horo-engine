#pragma once

/**
 * @file RuntimeSceneDefinition.h
 * @brief Immutable validated handoff from authoring data to runtime scene construction.
 */

#include "Horo/AI/AISceneComponents.h"
#include "Horo/Assets/AssetDependency.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Gameplay/Component.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Scene/CharacterSceneComponents.h"
#include "Horo/Runtime/Scene/NavigationSceneComponents.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/PrimitiveMeshDescriptor.h"
#include "Horo/Runtime/Scene/SceneComponents.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /**
     * @brief Typed core component payload owned by a definition or runtime scene.
     *
     * Authoring conveniences such as TriggerVolumeComponent are normalized by
     * scene conversion before this runtime-facing payload is constructed.
     */
    struct RuntimeComponentSet {
        std::optional<CameraComponent> camera;
        std::optional<LightComponent> light;
        std::optional<AudioSourceComponent> audioSource;
        std::optional<AudioListenerComponent> audioListener;
        std::optional<UiCanvasComponent> uiCanvas; /**< Optional canvas asset instantiated by the Runtime UI owner. */
        std::optional<NavigationSurfaceComponent> navigationSurface;
        std::optional<NavigationRegionComponent> navigationRegion;
        std::optional<NavigationModifierComponent> navigationModifier;
        std::optional<NavigationLinkComponent> navigationLink;
        std::optional<NavigationAgentComponent> navigationAgent;
        std::optional<AI::AiAgentComponent> aiAgent;
        std::optional<AI::AiControllerComponent> aiController;
        std::optional<CharacterControllerComponent> characterController;
        std::optional<RigidBodyComponent> rigidBody;
        std::vector<ColliderComponent> colliders;
        std::vector<PhysicsConstraintComponent> physicsConstraints;
        std::vector<Gameplay::BehaviorComponent> behaviors;
        std::vector<Gameplay::SerializedComponent> gameplayComponents;
        [[nodiscard]] bool operator==(const RuntimeComponentSet &) const noexcept = default;
    };

    /** @brief Complete immutable definition of one authored runtime entity. */
    struct RuntimeEntityDefinition {
        SceneObjectId object;
        std::optional<SceneObjectId> parent;
        Math::Transform localTransform;
        std::optional<PrimitiveMeshDescriptor> primitiveMesh;
        RuntimeComponentSet components;
    };

    /** @brief Shared typed asset requirement accepted by runtime-scene definitions. */
    using SceneAssetDependency = Assets::AssetDependency;

    /** @brief Portable project authority for converting legacy trigger authoring values.
     * @details These are validation constraints, never replacement collider behavior. Runtime world/filter
     * generations and leases are deliberately absent; the installed schema must have identical semantics.
     */
    struct TriggerVolumeProjectBinding final {
        std::uint64_t collisionSchemaFingerprint{}; /**< Semantic identity of the validated project collision schema. */
        Physics::CollisionProfileId profile;        /**< Exact selected sensor profile; no default substitution. */
        SceneAssetDependency material;              /**< Real project-owned cooked material requirement. */
        std::uint64_t materialSourceRevision{};     /**< Portable authored revision, never a runtime lease epoch. */
        Sha256Digest materialPayloadDigest;         /**< Digest of immutable cooked payload bytes, excluding the artifact envelope. */
        [[nodiscard]] bool operator==(const TriggerVolumeProjectBinding &) const noexcept = default;
    };

    /** @brief Binds portable migration proof to one canonical producer pair; component values remain authoritative. */
    struct TriggerVolumeMigrationEvidence final {
        SceneObjectId object;
        PhysicsBodySlotId body;
        PhysicsColliderSlotId collider;
        TriggerVolumeProjectBinding binding;
        [[nodiscard]] bool operator==(const TriggerVolumeMigrationEvidence &) const noexcept = default;
    };

    /** @brief Validated immutable runtime-scene construction input. */
    class RuntimeSceneDefinition final {
    public:
        /** @brief Returns the stable logical scene identity. */
        [[nodiscard]] SceneDefinitionId Id() const noexcept;
        /** @brief Returns the authored content revision represented by this definition. */
        [[nodiscard]] SceneDefinitionRevision Revision() const noexcept;
        /** @brief Returns every entity definition in stable authored order. */
        [[nodiscard]] std::span<const RuntimeEntityDefinition> Entities() const noexcept;
        /** @brief Returns required assets in canonical AssetId order. */
        [[nodiscard]] std::span<const SceneAssetDependency> AssetDependencies() const noexcept;
        /** @brief Returns canonical object-ordered legacy migration constraints retained through cook and load. */
        [[nodiscard]] std::span<const TriggerVolumeMigrationEvidence> TriggerVolumeEvidence() const noexcept;

    private:
        friend class SceneDefinitionBuilder;
        RuntimeSceneDefinition(SceneDefinitionId id, SceneDefinitionRevision revision, std::vector<RuntimeEntityDefinition> entities,
                               std::vector<SceneAssetDependency> assetDependencies,
                               std::vector<TriggerVolumeMigrationEvidence> triggerVolumeEvidence) noexcept;

        SceneDefinitionId id_;
        SceneDefinitionRevision revision_;
        std::vector<RuntimeEntityDefinition> entities_;
        std::vector<SceneAssetDependency> assetDependencies_;
        std::vector<TriggerVolumeMigrationEvidence> triggerVolumeEvidence_;
    };

    /** @brief Mutable load-time builder that validates before producing an immutable definition. */
    class SceneDefinitionBuilder final {
    public:
        /** @brief Creates a builder for one logical scene and authored revision. @param id Non-zero logical scene identity.
         * @param revision Authored content revision. */
        SceneDefinitionBuilder(SceneDefinitionId id, SceneDefinitionRevision revision) noexcept;
        /** @brief Appends one typed entity definition in stable authored order. @param entity Complete authored entity
         * payload. */
        void Add(RuntimeEntityDefinition entity);
        /** @brief Adds one required cooked asset, deduplicating an identical requirement. @param dependency Stable asset
         * identity and expected type. @return Success, or a typed error for an invalid or conflicting requirement. */
        [[nodiscard]] Result<void> RequireAsset(SceneAssetDependency dependency);
        /** @brief Adds exact migration constraints, rejecting duplicate objects and conflicting project evidence.
         * @param evidence Owned portable proof naming a canonical producer pair.
         * @return Success or InvalidDefinition; Build verifies component correspondence and requires the material asset.
         */
        [[nodiscard]] Result<void> RequireTriggerVolume(TriggerVolumeMigrationEvidence evidence);
        /** @brief Validates authored values and projects enabled navigation surface AssetIds as core.navmesh requirements.
         * @return Immutable definition
         * or the first typed validation error. */
        [[nodiscard]] Result<RuntimeSceneDefinition> Build() &&;

    private:
        SceneDefinitionId id_;
        SceneDefinitionRevision revision_;
        std::vector<RuntimeEntityDefinition> entities_;
        std::vector<SceneAssetDependency> assetDependencies_;
        std::vector<TriggerVolumeMigrationEvidence> triggerVolumeEvidence_;
    };

    /** @brief Validates one runtime entity payload independently of hierarchy membership. @param entity Entity payload to
     * validate. @return Success or a typed payload error. */
    [[nodiscard]] Result<void> ValidateRuntimeEntityDefinition(const RuntimeEntityDefinition &entity);
}  // namespace Horo::Runtime
