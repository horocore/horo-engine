#pragma once

#include "editor/document/SceneDocumentInternalCore.h"

namespace Horo::Editor::SceneDocumentDetail {
    [[nodiscard]] inline std::size_t EstimateBehaviorMemoryBytes(const std::vector<Gameplay::BehaviorComponent> &behaviors) noexcept {
        std::size_t total = behaviors.size() * sizeof(Gameplay::BehaviorComponent);
        for (const Gameplay::BehaviorComponent &behavior : behaviors) {
            total += behavior.typeId.Value().size();
            total += behavior.fields.size() * sizeof(Gameplay::BehaviorField);
            for (const Gameplay::BehaviorField &field : behavior.fields) {
                total += field.name.size();
                if (const auto *text = std::get_if<std::string>(&field.value))
                    total += text->size();
            }
        }
        return total;
    }

    [[nodiscard]] inline std::size_t EstimateNavigationComponentMemoryBytes(const SceneObjectComponentSet &components) noexcept {
        const std::size_t surfaceProfiles =
            components.navigationSurface ? components.navigationSurface->profiles.size() * sizeof(Navigation::NavigationAgentProfileId)
                                         : 0U;
        const std::size_t linkProfiles =
            components.navigationLink ? components.navigationLink->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
        return surfaceProfiles + linkProfiles;
    }

    [[nodiscard]] inline std::size_t EstimateSerializedComponentMemoryBytes(
        const std::vector<Gameplay::SerializedComponent> &components) noexcept {
        std::size_t bytes = components.size() * sizeof(Gameplay::SerializedComponent);
        for (const Gameplay::SerializedComponent &component : components)
            bytes += component.typeId.Value().size() + component.payload.size();
        return bytes;
    }

    [[nodiscard]] inline std::size_t EstimateSceneObjectOwnedMemoryBytes(const SceneObjectSnapshot &object) noexcept {
        const SceneObjectComponentSet &components = object.components;
        std::size_t bytes = object.name.size() + EstimateBehaviorMemoryBytes(components.behaviors) +
                            EstimateNavigationComponentMemoryBytes(components) +
                            components.colliders.size() * sizeof(Runtime::ColliderComponent) +
                            components.physicsConstraints.size() * sizeof(Runtime::PhysicsConstraintComponent) +
                            EstimateSerializedComponentMemoryBytes(components.gameplayComponents);
        for (const Runtime::ColliderComponent &collider : components.colliders)
            bytes += collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding);
        return bytes;
    }

    template <typename Delta> [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const Delta &) noexcept {
        return sizeof(Delta);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const CreatedObjectDelta &delta) noexcept {
        return sizeof(delta) + EstimateSceneObjectOwnedMemoryBytes(delta.object);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const RenamedObjectDelta &delta) noexcept {
        return sizeof(delta) + delta.before.size() + delta.after.size();
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const DeletedObjectsDelta &delta) noexcept {
        std::size_t bytes = sizeof(delta) + delta.roots.size() * sizeof(SceneObjectId) + delta.objects.size() * sizeof(IndexedSceneObject);
        for (const IndexedSceneObject &object : delta.objects)
            bytes += EstimateSceneObjectOwnedMemoryBytes(object.object);
        return bytes + delta.prefabInstances.size() * sizeof(IndexedPrefabInstance);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const DeletedPrefabInstancesDelta &delta) noexcept {
        return sizeof(delta) + delta.instances.size() * sizeof(IndexedPrefabInstance);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const TransformedObjectsDelta &delta) noexcept {
        return sizeof(delta) + delta.objects.size() * sizeof(TransformedObjectDelta);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const BehaviorsChangedDelta &delta) noexcept {
        return sizeof(delta) + EstimateBehaviorMemoryBytes(delta.before) + EstimateBehaviorMemoryBytes(delta.after);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const GameplayComponentsChangedDelta &delta) noexcept {
        return sizeof(delta) + EstimateSerializedComponentMemoryBytes(delta.before) + EstimateSerializedComponentMemoryBytes(delta.after);
    }

    [[nodiscard]] inline std::size_t EstimateTypedDeltaMemoryBytes(const NavigationComponentsChangedDelta &delta) noexcept {
        const auto profileBytes = [](const std::optional<Runtime::NavigationSurfaceComponent> &surface) {
            return surface ? surface->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
        };
        const auto linkProfileBytes = [](const std::optional<Runtime::NavigationLinkComponent> &link) {
            return link ? link->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
        };
        return sizeof(delta) + profileBytes(delta.surfaceBefore) + profileBytes(delta.surfaceAfter) + linkProfileBytes(delta.linkBefore) +
               linkProfileBytes(delta.linkAfter) + (delta.agentBefore.has_value() ? sizeof(Runtime::NavigationAgentComponent) : 0U) +
               (delta.agentAfter.has_value() ? sizeof(Runtime::NavigationAgentComponent) : 0U);
    }

    [[nodiscard]] inline std::size_t EstimateMemoryBytes(const SceneCommandDelta &delta, const std::size_t affectedObjectCount) noexcept {
        return affectedObjectCount * sizeof(SceneObjectId) + std::visit([]<typename Delta>(const Delta &typedDelta) {
            return EstimateTypedDeltaMemoryBytes(typedDelta);
        }, delta);
    }

    [[nodiscard]] inline SceneObjectId DeltaRootObject(const SceneCommandDelta &delta) noexcept {
        return std::visit([]<typename Delta>(const Delta &typedDelta) {
            if constexpr (std::is_same_v<Delta, CreatedObjectDelta>) {
                return typedDelta.object.id;
            } else if constexpr (std::is_same_v<Delta, DeletedObjectsDelta>) {
                return typedDelta.roots.empty() ? SceneObjectId{} : typedDelta.roots.front();
            } else if constexpr (std::is_same_v<Delta, TransformedObjectsDelta>) {
                return typedDelta.objects.empty() ? SceneObjectId{} : typedDelta.objects.front().object;
            } else if constexpr (std::is_same_v<Delta, CreatedPrefabInstanceDelta> || std::is_same_v<Delta, PrefabInstanceTransformDelta> ||
                                 std::is_same_v<Delta, PrefabInstanceReparentDelta> || std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                return SceneObjectId{};
            } else {
                return typedDelta.object;
            }
        }, delta);
    }

    [[nodiscard]] inline std::optional<Prefab::PrefabInstanceId> DeltaRootPrefabInstance(const SceneCommandDelta &delta) noexcept {
        return std::visit([]<typename Delta>(const Delta &typedDelta) -> std::optional<Prefab::PrefabInstanceId> {
            if constexpr (std::is_same_v<Delta, CreatedPrefabInstanceDelta>) {
                return typedDelta.instance.instanceId;
            } else if constexpr (std::is_same_v<Delta, PrefabInstanceTransformDelta> ||
                                 std::is_same_v<Delta, PrefabInstanceReparentDelta>) {
                return typedDelta.instance;
            } else if constexpr (std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                return typedDelta.instances.empty() ? std::nullopt : std::optional{typedDelta.instances.front().instance.instanceId};
            } else {
                return std::nullopt;
            }
        }, delta);
    }

    inline void AddComponent(SceneObjectComponentSet &components, const ComponentType type) {
        using enum ComponentType;
        switch (type) {
            case Camera:
                components.camera = Runtime::CameraComponent{};
                break;
            case Light:
                components.light = Runtime::LightComponent{};
                break;
            case TriggerVolume:
                components.triggerVolume = Runtime::TriggerVolumeComponent{};
                break;
            case AudioSource:
                components.audioSource = Runtime::AudioSourceComponent{};
                break;
            default:
                break;
        }
    }

    [[nodiscard]] inline bool HasComponent(const SceneObjectComponentSet &components, const ComponentType type) noexcept {
        using enum ComponentType;
        switch (type) {
            case Camera:
                return components.camera.has_value();
            case Light:
                return components.light.has_value();
            case TriggerVolume:
                return components.triggerVolume.has_value();
            case AudioSource:
                return components.audioSource.has_value();
            default:
                return false;
        }
    }

    inline void RemoveComponent(SceneObjectComponentSet &components, const ComponentType type) {
        using enum ComponentType;
        switch (type) {
            case Camera:
                components.camera = std::nullopt;
                break;
            case Light:
                components.light = std::nullopt;
                break;
            case TriggerVolume:
                components.triggerVolume = std::nullopt;
                break;
            case AudioSource:
                components.audioSource = std::nullopt;
                break;
            default:
                break;
        }
    }

    inline void RestoreComponent(SceneObjectComponentSet &components, const ComponentRemovedDelta &delta) {
        using enum ComponentType;
        switch (delta.type) {
            case Camera:
                components.camera = delta.camera;
                break;
            case Light:
                components.light = delta.light;
                break;
            case TriggerVolume:
                components.triggerVolume = delta.triggerVolume;
                break;
            case AudioSource:
                components.audioSource = delta.audioSource;
                break;
            default:
                break;
        }
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CreatedObjectDelta &delta) {
        const std::size_t index = std::min(delta.index, objects.size());
        objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), delta.object);
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const RenamedObjectDelta &delta) {
        FindObject(objects, delta.object)->name = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectDelta &delta) {
        FindObject(objects, delta.object)->localTransform = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectsDelta &delta) {
        for (const TransformedObjectDelta &object : delta.objects)
            FindObject(objects, object.object)->localTransform = object.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CameraChangedDelta &delta) {
        FindObject(objects, delta.object)->components.camera = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const LightChangedDelta &delta) {
        FindObject(objects, delta.object)->components.light = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TriggerVolumeChangedDelta &delta) {
        FindObject(objects, delta.object)->components.triggerVolume = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const AudioSourceChangedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            object->components.audioSource = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const NavigationComponentsChangedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end()) {
            object->components.navigationSurface = delta.surfaceAfter;
            object->components.navigationRegion = delta.regionAfter;
            object->components.navigationModifier = delta.modifierAfter;
            object->components.navigationLink = delta.linkAfter;
            object->components.navigationAgent = delta.agentAfter;
        }
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const EditorStateChangedDelta &delta) {
        FindObject(objects, delta.object)->editorState = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const BehaviorsChangedDelta &delta) {
        FindObject(objects, delta.object)->components.behaviors = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const GameplayComponentsChangedDelta &delta) {
        FindObject(objects, delta.object)->components.gameplayComponents = delta.after;
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentAddedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            AddComponent(object->components, delta.type);
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentRemovedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            RemoveComponent(object->components, delta.type);
    }

    inline void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const DeletedObjectsDelta &delta) {
        std::erase_if(objects, [&delta](const SceneObjectSnapshot &object) {
            return std::ranges::any_of(delta.objects, [&object](const IndexedSceneObject &removed) {
                return removed.object.id == object.id;
            });
        });
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
        const std::size_t index = std::min(delta.index, instances.size());
        instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(index), delta.instance);
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->rootTransform = delta.after;
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceReparentDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->parent = delta.after;
    }

    inline void RemovePrefabInstances(std::vector<ScenePrefabInstance> &instances,
                                      const std::vector<IndexedPrefabInstance> &removedInstances) {
        std::erase_if(instances, [&removedInstances](const ScenePrefabInstance &instance) {
            return std::ranges::any_of(removedInstances, [&instance](const IndexedPrefabInstance &removed) {
                return removed.instance.instanceId == instance.instanceId;
            });
        });
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedPrefabInstancesDelta &delta) {
        RemovePrefabInstances(instances, delta.instances);
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedObjectsDelta &delta) {
        RemovePrefabInstances(instances, delta.prefabInstances);
    }

    inline void ApplyDelta(std::vector<SceneObjectSnapshot> &objects, const SceneCommandDelta &delta) {
        std::visit([&objects]<typename Delta>(const Delta &typedDelta) {
            if constexpr (!std::is_same_v<Delta, CreatedPrefabInstanceDelta> && !std::is_same_v<Delta, PrefabInstanceTransformDelta> &&
                          !std::is_same_v<Delta, PrefabInstanceReparentDelta> && !std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                ApplyTypedDelta(objects, typedDelta);
            }
        }, delta);
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CreatedObjectDelta &delta) {
        std::erase_if(objects, [&delta](const SceneObjectSnapshot &object) {
            return object.id == delta.object.id;
        });
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const RenamedObjectDelta &delta) {
        FindObject(objects, delta.object)->name = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectDelta &delta) {
        FindObject(objects, delta.object)->localTransform = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectsDelta &delta) {
        for (const TransformedObjectDelta &object : delta.objects)
            FindObject(objects, object.object)->localTransform = object.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CameraChangedDelta &delta) {
        FindObject(objects, delta.object)->components.camera = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const LightChangedDelta &delta) {
        FindObject(objects, delta.object)->components.light = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TriggerVolumeChangedDelta &delta) {
        FindObject(objects, delta.object)->components.triggerVolume = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const AudioSourceChangedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            object->components.audioSource = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const NavigationComponentsChangedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end()) {
            object->components.navigationSurface = delta.surfaceBefore;
            object->components.navigationRegion = delta.regionBefore;
            object->components.navigationModifier = delta.modifierBefore;
            object->components.navigationLink = delta.linkBefore;
            object->components.navigationAgent = delta.agentBefore;
        }
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const EditorStateChangedDelta &delta) {
        FindObject(objects, delta.object)->editorState = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const BehaviorsChangedDelta &delta) {
        FindObject(objects, delta.object)->components.behaviors = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const GameplayComponentsChangedDelta &delta) {
        FindObject(objects, delta.object)->components.gameplayComponents = delta.before;
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentAddedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            RemoveComponent(object->components, delta.type);
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentRemovedDelta &delta) {
        if (const auto object = FindObject(objects, delta.object); object != objects.end())
            RestoreComponent(object->components, delta);
    }

    inline void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const DeletedObjectsDelta &delta) {
        for (const IndexedSceneObject &removed : delta.objects) {
            const std::size_t index = std::min(removed.index, objects.size());
            objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), removed.object);
        }
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
        std::erase_if(instances, [&delta](const ScenePrefabInstance &instance) {
            return instance.instanceId == delta.instance.instanceId;
        });
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->rootTransform = delta.before;
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceReparentDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->parent = delta.before;
    }

    inline void RestorePrefabInstances(std::vector<ScenePrefabInstance> &instances,
                                       const std::vector<IndexedPrefabInstance> &removedInstances) {
        for (const IndexedPrefabInstance &removed : removedInstances) {
            const std::size_t index = std::min(removed.index, instances.size());
            instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(index), removed.instance);
        }
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedPrefabInstancesDelta &delta) {
        RestorePrefabInstances(instances, delta.instances);
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedObjectsDelta &delta) {
        RestorePrefabInstances(instances, delta.prefabInstances);
    }

    template <bool Revert>
    inline void MutateDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
                            const SceneCommandDelta &delta) {
        std::visit([&objects, &instances]<typename Delta>(const Delta &typedDelta) {
            if constexpr (std::is_same_v<Delta, CreatedPrefabInstanceDelta> || std::is_same_v<Delta, PrefabInstanceTransformDelta> ||
                          std::is_same_v<Delta, PrefabInstanceReparentDelta> || std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                if constexpr (Revert)
                    RevertPrefabTypedDelta(instances, typedDelta);
                else
                    ApplyPrefabTypedDelta(instances, typedDelta);
            } else {
                if constexpr (Revert)
                    RevertTypedDelta(objects, typedDelta);
                else
                    ApplyTypedDelta(objects, typedDelta);
                if constexpr (std::is_same_v<Delta, DeletedObjectsDelta>) {
                    if constexpr (Revert)
                        RevertPrefabTypedDelta(instances, typedDelta);
                    else
                        ApplyPrefabTypedDelta(instances, typedDelta);
                }
            }
        }, delta);
    }

    inline void ApplyDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
                           const SceneCommandDelta &delta) {
        MutateDelta<false>(objects, instances, delta);
    }

    inline void RevertDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
                            const SceneCommandDelta &delta) {
        MutateDelta<true>(objects, instances, delta);
    }

}  // namespace Horo::Editor::SceneDocumentDetail
