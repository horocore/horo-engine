#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &code, std::string message) {
            return Result<void>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] bool ValidPrimitive(const PrimitiveMeshDescriptor &primitive) noexcept {
            if (primitive.version.value != 1 || primitive.parameters.index() != static_cast<std::size_t>(primitive.type))
                return false;
            return std::visit([]<typename T>(const T &value) {
                if constexpr (std::is_same_v<T, BoxMeshParameters>)
                    return Math::IsFinite(value.size) && value.size.x > 0 && value.size.y > 0 && value.size.z > 0;
                else if constexpr (std::is_same_v<T, SphereMeshParameters>)
                    return std::isfinite(value.radius) && value.radius > 0 && value.slices >= 3 && value.stacks >= 2;
                else if constexpr (std::is_same_v<T, CapsuleMeshParameters>)
                    return std::isfinite(value.radius) && std::isfinite(value.totalHeight) && value.radius > 0 &&
                           value.totalHeight >= value.radius * 2 && value.radialSegments >= 3 && value.hemisphereRings >= 1;
                else if constexpr (std::is_same_v<T, CylinderMeshParameters> || std::is_same_v<T, ConeMeshParameters>)
                    return std::isfinite(value.radius) && std::isfinite(value.height) && value.radius > 0 && value.height > 0 &&
                           value.radialSegments >= 3;
                else
                    return Math::IsFinite(value.size) && value.size.x > 0 && value.size.y > 0;
            }, primitive.parameters);
        }

        /** @brief Validates camera values without adding camera-specific branching to component-set validation. */
        [[nodiscard]] bool ValidCamera(const CameraComponent &camera) noexcept {
            return std::isfinite(camera.verticalFieldOfViewRadians) && std::isfinite(camera.orthographicHeight) &&
                   std::isfinite(camera.nearPlane) && std::isfinite(camera.farPlane) && camera.nearPlane > 0 &&
                   camera.farPlane > camera.nearPlane &&
                   (camera.projection != CameraProjection::Perspective ||
                    (camera.verticalFieldOfViewRadians > 0 && camera.verticalFieldOfViewRadians < Math::Pi)) &&
                   (camera.projection != CameraProjection::Orthographic || camera.orthographicHeight > 0);
        }

        /** @brief Validates light values without adding light-specific branching to component-set validation. */
        [[nodiscard]] bool ValidLight(const LightComponent &light) noexcept {
            return Math::IsFinite(light.color) && std::isfinite(light.intensity) && std::isfinite(light.range) &&
                   std::isfinite(light.innerConeRadians) && std::isfinite(light.outerConeRadians) && light.intensity >= 0 &&
                   light.color.x >= 0 && light.color.y >= 0 && light.color.z >= 0 && light.range >= 0 && light.innerConeRadians >= 0 &&
                   light.outerConeRadians >= light.innerConeRadians && light.outerConeRadians <= Math::Pi;
        }

        /** @brief Validates behavior payloads and enforces unique instance identities. */
        [[nodiscard]] bool ValidBehaviors(const std::span<const Gameplay::BehaviorComponent> behaviors) {
            std::vector<Gameplay::BehaviorInstanceId> behaviorIds;
            behaviorIds.reserve(behaviors.size());
            for (const Gameplay::BehaviorComponent &behavior : behaviors) {
                if (Gameplay::ValidateBehaviorComponent(behavior).HasError() ||
                    std::ranges::find(behaviorIds, behavior.instanceId) != behaviorIds.end())
                    return false;
                behaviorIds.push_back(behavior.instanceId);
            }
            return true;
        }

        [[nodiscard]] bool ValidComponents(const RuntimeComponentSet &components) noexcept {
            if (components.camera && !ValidCamera(*components.camera))
                return false;
            if (components.light && !ValidLight(*components.light))
                return false;
            if (components.audioSource && (Audio::ValidateAudioSoundReference(components.audioSource->sound).HasError() ||
                                           Audio::ValidateAudioSoundPlaybackDefaults(components.audioSource->playback).HasError() ||
                                           Audio::ValidateAudioSceneLifecyclePolicy(components.audioSource->sceneLifecycle).HasError()))
                return false;
            if (components.uiCanvas && Ui::ValidateUiCanvasAssetReference(components.uiCanvas->canvas).HasError())
                return false;
            if (components.navigationSurface && ValidateNavigationSurfaceComponent(*components.navigationSurface).HasError())
                return false;
            if (components.navigationRegion && ValidateNavigationRegionComponent(*components.navigationRegion).HasError())
                return false;
            if (components.navigationModifier && ValidateNavigationModifierComponent(*components.navigationModifier).HasError())
                return false;
            if (components.navigationLink && ValidateNavigationLinkComponent(*components.navigationLink).HasError())
                return false;
            return ValidBehaviors(components.behaviors);
        }

        /** @brief Validates navigation identities and cross-component references across the complete scene. */
        [[nodiscard]] Result<void> ValidateNavigationComponents(const std::span<const RuntimeEntityDefinition> entities) {
            std::vector<NavigationSceneComponentView> navigationComponents;
            navigationComponents.reserve(entities.size());
            for (const RuntimeEntityDefinition &entity : entities) {
                navigationComponents.push_back(
                    {.surface = entity.components.navigationSurface ? &*entity.components.navigationSurface : nullptr,
                     .region = entity.components.navigationRegion ? &*entity.components.navigationRegion : nullptr,
                     .modifier = entity.components.navigationModifier ? &*entity.components.navigationModifier : nullptr,
                     .link = entity.components.navigationLink ? &*entity.components.navigationLink : nullptr});
            }
            return ValidateNavigationSceneComponentViews(navigationComponents);
        }

        /** @brief Validates Physics identities and explicit body references across the complete scene. */
        [[nodiscard]] Result<void> ValidatePhysicsComponents(const std::span<const RuntimeEntityDefinition> entities) {
            std::vector<PhysicsSceneComponentView> physicsComponents;
            physicsComponents.reserve(entities.size());
            for (const RuntimeEntityDefinition &entity : entities) {
                physicsComponents.push_back({.owner = entity.object,
                                             .rigidBody = entity.components.rigidBody ? &*entity.components.rigidBody : nullptr,
                                             .colliders = entity.components.colliders,
                                             .constraints = entity.components.physicsConstraints});
            }
            return ValidatePhysicsSceneComponentViews(physicsComponents);
        }

        /** @brief Validates all authored subsystem projections before publishing a definition. */
        [[nodiscard]] Result<void> ValidateAuthoredComponents(const std::span<const RuntimeEntityDefinition> entities) {
            if (Result<void> navigation = ValidateNavigationComponents(entities); navigation.HasError())
                return navigation;
            return ValidatePhysicsComponents(entities);
        }

        [[nodiscard]] Result<void> ValidateEntityHierarchy(const std::vector<RuntimeEntityDefinition> &entities) {
            std::unordered_map<std::uint64_t, std::size_t> indices;
            indices.reserve(entities.size());
            for (std::size_t index = 0; index < entities.size(); ++index) {
                if (const Result<void> valid = ValidateRuntimeEntityDefinition(entities[index]); valid.HasError())
                    return valid;
                if (!indices.emplace(entities[index].object.value, index).second)
                    return Failure(SceneErrors::DuplicateObject, "Runtime scene contains a duplicate authored object identity.");
            }

            std::vector<std::optional<std::size_t>> parents(entities.size());
            for (std::size_t index = 0; index < entities.size(); ++index) {
                if (!entities[index].parent)
                    continue;
                const auto parent = indices.find(entities[index].parent->value);
                if (parent == indices.end())
                    return Failure(SceneErrors::ParentNotFound, "Runtime scene entity references a missing parent.");
                parents[index] = parent->second;
            }

            enum class Visit : std::uint8_t {
                Unvisited,
                Visiting,
                Complete
            };
            std::vector visits(entities.size(), Visit::Unvisited);
            std::function<bool(std::size_t)> visit = [&](const std::size_t index) {
                using enum Visit;
                if (visits[index] == Complete)
                    return true;
                if (visits[index] == Visiting)
                    return false;
                visits[index] = Visiting;
                if (parents[index].has_value() && !visit(*parents[index]))
                    return false;
                visits[index] = Complete;
                return true;
            };

            for (std::size_t index = 0; index < entities.size(); ++index)
                if (!visit(index))
                    return Failure(SceneErrors::HierarchyCycle, "Runtime scene hierarchy contains a cycle.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc RuntimeSceneDefinition::RuntimeSceneDefinition */
    RuntimeSceneDefinition::RuntimeSceneDefinition(SceneDefinitionId id, SceneDefinitionRevision revision,
                                                   std::vector<RuntimeEntityDefinition> entities,
                                                   std::vector<SceneAssetDependency> assetDependencies) noexcept
        : id_(id), revision_(revision), entities_(std::move(entities)), assetDependencies_(std::move(assetDependencies)) {}

    /** @copydoc RuntimeSceneDefinition::Id */
    SceneDefinitionId RuntimeSceneDefinition::Id() const noexcept {
        return id_;
    }

    /** @copydoc RuntimeSceneDefinition::Revision */
    SceneDefinitionRevision RuntimeSceneDefinition::Revision() const noexcept {
        return revision_;
    }

    /** @copydoc RuntimeSceneDefinition::Entities */
    std::span<const RuntimeEntityDefinition> RuntimeSceneDefinition::Entities() const noexcept {
        return entities_;
    }

    /** @copydoc RuntimeSceneDefinition::AssetDependencies */
    std::span<const SceneAssetDependency> RuntimeSceneDefinition::AssetDependencies() const noexcept {
        return assetDependencies_;
    }

    /** @copydoc SceneDefinitionBuilder::SceneDefinitionBuilder */
    SceneDefinitionBuilder::SceneDefinitionBuilder(SceneDefinitionId id, SceneDefinitionRevision revision) noexcept
        : id_(id), revision_(revision) {}

    /** @copydoc SceneDefinitionBuilder::Add */
    void SceneDefinitionBuilder::Add(RuntimeEntityDefinition entity) {
        entities_.push_back(std::move(entity));
    }

    /** @copydoc SceneDefinitionBuilder::RequireAsset */
    Result<void> SceneDefinitionBuilder::RequireAsset(SceneAssetDependency dependency) {
        if (!dependency.id.IsValid() || dependency.expectedType.Value().empty())
            return Failure(SceneErrors::InvalidAssetDependency,
                           "Runtime scene asset dependency must have a non-zero identity and valid type.");
        const auto found = std::ranges::find(assetDependencies_, dependency.id, [](const SceneAssetDependency &value) {
            return value.id;
        });
        if (found == assetDependencies_.end()) {
            assetDependencies_.push_back(std::move(dependency));
            return Result<void>::Success();
        }
        if (found->expectedType != dependency.expectedType)
            return Failure(SceneErrors::ConflictingAssetDependency, "Runtime scene requires one asset identity with conflicting types.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateRuntimeEntityDefinition */
    Result<void> ValidateRuntimeEntityDefinition(const RuntimeEntityDefinition &entity) {
        if (!entity.object.IsValid())
            return Failure(SceneErrors::InvalidEntity, "Runtime entity has an invalid authored object identity.");
        if (entity.localTransform.TryToMatrix().HasError())
            return Failure(SceneErrors::InvalidEntity, "Runtime entity transform is not finite.");
        if (entity.primitiveMesh && !ValidPrimitive(*entity.primitiveMesh))
            return Failure(SceneErrors::InvalidEntity, "Runtime entity primitive descriptor is invalid.");
        if (!ValidComponents(entity.components))
            return Failure(SceneErrors::InvalidEntity, "Runtime entity component payload is invalid.");
        return Result<void>::Success();
    }

    /** @copydoc SceneDefinitionBuilder::Build */
    Result<RuntimeSceneDefinition> SceneDefinitionBuilder::Build() && {
        if (!id_.IsValid())
            return Result<RuntimeSceneDefinition>::Failure(
                MakeError(SceneErrors::InvalidDefinition, "Runtime scene identity must be non-zero."));

        if (const Result<void> valid = ValidateEntityHierarchy(entities_); valid.HasError())
            return Result<RuntimeSceneDefinition>::Failure(valid.ErrorValue());

        std::ranges::sort(assetDependencies_, {}, [](const SceneAssetDependency &dependency) {
            return dependency.id;
        });
        if (Result<void> components = ValidateAuthoredComponents(entities_); components.HasError())
            return Result<RuntimeSceneDefinition>::Failure(components.ErrorValue());

        return Result<RuntimeSceneDefinition>::Success(
            RuntimeSceneDefinition{id_, revision_, std::move(entities_), std::move(assetDependencies_)});
    }
}  // namespace Horo::Runtime
