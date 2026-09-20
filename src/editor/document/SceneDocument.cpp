#include "editor/document/SceneDocument.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/project_model/EditorModelErrors.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <limits>
#include <memory>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace Horo::Editor {
    /** @copydoc IsValidSceneObjectName */
    bool IsValidSceneObjectName(const std::string_view name) noexcept {
        return !name.empty() && name.size() <= MaximumSceneObjectNameBytes;
    }

    /** @copydoc IsValidCameraComponent */
    bool IsValidCameraComponent(const Runtime::CameraComponent &camera) noexcept {
        using enum Runtime::CameraProjection;
        const bool projectionValid = camera.projection == Perspective || camera.projection == Orthographic;
        if (const bool commonValuesValid = std::isfinite(camera.verticalFieldOfViewRadians) && std::isfinite(camera.orthographicHeight) &&
                                           std::isfinite(camera.nearPlane) && std::isfinite(camera.farPlane) && camera.nearPlane > 0.0F &&
                                           camera.farPlane > camera.nearPlane;
            !projectionValid || !commonValuesValid)
            return false;
        if (camera.projection == Perspective) {
            return camera.verticalFieldOfViewRadians > 0.0F && camera.verticalFieldOfViewRadians < Math::Pi;
        }
        return camera.orthographicHeight > 0.0F;
    }

    /** @copydoc IsValidLightComponent */
    bool IsValidLightComponent(const Runtime::LightComponent &light) noexcept {
        using enum Runtime::LightKind;
        const bool kindValid = light.kind == Directional || light.kind == Point || light.kind == Spot;
        return kindValid && Math::IsFinite(light.color) && light.color.x >= 0.0F && light.color.y >= 0.0F && light.color.z >= 0.0F &&
               std::isfinite(light.intensity) && light.intensity >= 0.0F && std::isfinite(light.range) && light.range >= 0.0F &&
               std::isfinite(light.innerConeRadians) && light.innerConeRadians >= 0.0F && std::isfinite(light.outerConeRadians) &&
               light.outerConeRadians >= light.innerConeRadians && light.outerConeRadians <= Math::Pi;
    }

    /** @copydoc IsValidAudioSourceComponent */
    bool IsValidAudioSourceComponent(const Runtime::AudioSourceComponent &audioSource) noexcept {
        return Audio::ValidateAudioSoundReference(audioSource.sound).HasValue() &&
               Audio::ValidateAudioSoundPlaybackDefaults(audioSource.playback).HasValue();
    }

    /** @copydoc ResolveSceneObjectEditorState */
    std::optional<ResolvedSceneObjectEditorState> ResolveSceneObjectEditorState(const std::span<const SceneObjectSnapshot> objects,
                                                                                const SceneObjectId object) noexcept {
        const auto local = std::ranges::find(objects, object, &SceneObjectSnapshot::id);
        if (local == objects.end())
            return std::nullopt;

        ResolvedSceneObjectEditorState resolved{.local = local->editorState,
                                                .effectivelyVisible = local->editorState.visible,
                                                .effectivelyLocked = local->editorState.locked};
        std::optional<SceneObjectId> parent = local->parent;
        std::size_t remainingDepth = objects.size();
        while (parent.has_value()) {
            if (remainingDepth-- == 0)
                return std::nullopt;
            const auto ancestor = std::ranges::find(objects, *parent, &SceneObjectSnapshot::id);
            if (ancestor == objects.end())
                return std::nullopt;
            if (!ancestor->editorState.visible) {
                resolved.effectivelyVisible = false;
                resolved.hiddenByParent = true;
            }
            if (ancestor->editorState.locked) {
                resolved.effectivelyLocked = true;
                resolved.lockedByParent = true;
            }
            parent = ancestor->parent;
        }
        return resolved;
    }

    namespace {
        constexpr std::size_t kMaximumHistoryEntries = 256;
        constexpr std::size_t kMaximumHistoryBytes = 4U * 1024U * 1024U;

        struct CreatedObjectDelta {
            SceneObjectSnapshot object;
            std::size_t index{0};
            DocumentChangeKind kind{DocumentChangeKind::Created};
        };

        struct RenamedObjectDelta {
            SceneObjectId object;
            std::string before;
            std::string after;
        };

        struct TransformedObjectDelta {
            SceneObjectId object;
            Math::Transform before;
            Math::Transform after;
        };

        struct TransformedObjectsDelta {
            std::vector<TransformedObjectDelta> objects;
        };

        struct CameraChangedDelta {
            SceneObjectId object;
            Runtime::CameraComponent before;
            Runtime::CameraComponent after;
        };

        struct LightChangedDelta {
            SceneObjectId object;
            Runtime::LightComponent before;
            Runtime::LightComponent after;
        };

        struct TriggerVolumeChangedDelta {
            SceneObjectId object;
            Runtime::TriggerVolumeComponent before;
            Runtime::TriggerVolumeComponent after;
        };

        struct AudioSourceChangedDelta {
            SceneObjectId object;
            Runtime::AudioSourceComponent before;
            Runtime::AudioSourceComponent after;
        };

        struct NavigationComponentsChangedDelta {
            SceneObjectId object;
            std::optional<Runtime::NavigationSurfaceComponent> surfaceBefore;
            std::optional<Runtime::NavigationSurfaceComponent> surfaceAfter;
            std::optional<Runtime::NavigationRegionComponent> regionBefore;
            std::optional<Runtime::NavigationRegionComponent> regionAfter;
            std::optional<Runtime::NavigationModifierComponent> modifierBefore;
            std::optional<Runtime::NavigationModifierComponent> modifierAfter;
            std::optional<Runtime::NavigationLinkComponent> linkBefore;
            std::optional<Runtime::NavigationLinkComponent> linkAfter;
        };

        struct EditorStateChangedDelta {
            SceneObjectId object;
            SceneObjectEditorState before;
            SceneObjectEditorState after;
        };

        struct ComponentAddedDelta {
            SceneObjectId object;
            ComponentType type;
        };

        struct ComponentRemovedDelta {
            SceneObjectId object;
            ComponentType type;
            std::optional<Runtime::CameraComponent> camera;
            std::optional<Runtime::LightComponent> light;
            std::optional<Runtime::TriggerVolumeComponent> triggerVolume;
            std::optional<Runtime::AudioSourceComponent> audioSource;
        };

        struct BehaviorsChangedDelta {
            SceneObjectId object;
            std::vector<Gameplay::BehaviorComponent> before;
            std::vector<Gameplay::BehaviorComponent> after;
        };

        struct IndexedSceneObject {
            SceneObjectSnapshot object;
            std::size_t index{0};
        };

        struct CreatedPrefabInstanceDelta {
            ScenePrefabInstance instance;
            std::size_t index{0};
            DocumentChangeKind kind{DocumentChangeKind::PrefabInstanceCreated};
        };

        struct PrefabInstanceTransformDelta {
            Prefab::PrefabInstanceId instance;
            Math::Transform before;
            Math::Transform after;
        };

        struct PrefabInstanceReparentDelta {
            Prefab::PrefabInstanceId instance;
            std::optional<SceneObjectId> before;
            std::optional<SceneObjectId> after;
        };

        struct IndexedPrefabInstance {
            ScenePrefabInstance instance;
            std::size_t index{0};
        };

        struct DeletedPrefabInstancesDelta {
            std::vector<IndexedPrefabInstance> instances;
        };

        struct DeletedObjectsDelta {
            std::vector<SceneObjectId> roots;
            std::vector<IndexedSceneObject> objects;
            std::vector<IndexedPrefabInstance> prefabInstances;
        };

        using SceneCommandDelta =
            std::variant<CreatedObjectDelta, RenamedObjectDelta, TransformedObjectDelta, TransformedObjectsDelta, CameraChangedDelta,
                         LightChangedDelta, TriggerVolumeChangedDelta, AudioSourceChangedDelta, NavigationComponentsChangedDelta,
                         EditorStateChangedDelta, ComponentAddedDelta, ComponentRemovedDelta, BehaviorsChangedDelta, DeletedObjectsDelta,
                         CreatedPrefabInstanceDelta, PrefabInstanceTransformDelta, PrefabInstanceReparentDelta,
                         DeletedPrefabInstancesDelta>;

        struct HistoryRecord {
            DocumentStateId beforeState;
            DocumentStateId afterState;
            SceneCommandDelta delta;
            std::vector<SceneObjectId> affectedObjects;
            std::size_t memoryBytes{0};
            std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
        };

        [[nodiscard]] Error MakeDocumentError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsValid(const Math::Transform &transform) noexcept {
            const Math::Quaternion rotation = transform.rotation;
            const float rotationLengthSquared =
                rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
            return Math::IsFinite(transform.translation) && Math::IsFinite(transform.scale) && std::isfinite(rotation.x) &&
                   std::isfinite(rotation.y) && std::isfinite(rotation.z) && std::isfinite(rotation.w) && rotationLengthSquared > 0.0F;
        }

        [[nodiscard]] auto FindObject(std::vector<SceneObjectSnapshot> &objects, const SceneObjectId id) {
            return std::ranges::find(objects, id, &SceneObjectSnapshot::id);
        }

        [[nodiscard]] auto FindObject(const std::vector<SceneObjectSnapshot> &objects, const SceneObjectId id) {
            return std::ranges::find(objects, id, &SceneObjectSnapshot::id);
        }

        [[nodiscard]] auto FindPrefabInstance(std::vector<ScenePrefabInstance> &instances, const Prefab::PrefabInstanceId id) {
            return std::ranges::find(instances, id, &ScenePrefabInstance::instanceId);
        }

        [[nodiscard]] auto FindPrefabInstance(const std::vector<ScenePrefabInstance> &instances, const Prefab::PrefabInstanceId id) {
            return std::ranges::find(instances, id, &ScenePrefabInstance::instanceId);
        }

        [[nodiscard]] bool IsEffectivelyLocked(const std::vector<SceneObjectSnapshot> &objects, const SceneObjectId object) noexcept {
            const std::optional<ResolvedSceneObjectEditorState> state = ResolveSceneObjectEditorState(objects, object);
            return state.has_value() && state->effectivelyLocked;
        }

        [[nodiscard]] bool IsPrefabInstanceLocked(const std::vector<SceneObjectSnapshot> &objects,
                                                  const ScenePrefabInstance &instance) noexcept {
            return instance.parent.has_value() && IsEffectivelyLocked(objects, *instance.parent);
        }

        [[nodiscard]] Error LockedObjectError() {
            return MakeDocumentError(SceneDocumentErrors::ObjectLocked, "Scene object or one of its ancestors is locked in the editor.");
        }

        [[nodiscard]] Result<SceneCommandResult> ComponentNoOpResult(const SceneDocument &document, const SceneObjectId object) {
            return Result<SceneCommandResult>::Success(
                {object, document.Revision(), document.State(), DocumentChangeKind::ComponentChanged, {}, false});
        }

        [[nodiscard]] Result<void> ValidateDescriptor(const std::optional<PrimitiveMeshDescriptor> &descriptor) {
            if (!descriptor.has_value()) {
                return Result<void>::Success();
            }
            if (descriptor->version.value != 1 || Runtime::PrimitiveCatalog::Find(descriptor->type) == nullptr ||
                Runtime::PrimitiveMeshGenerator::Generate(*descriptor).HasError()) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidPrimitive, "Primitive mesh descriptor is not supported."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePhysicsAuthoringComponents(const SceneObjectComponentSet &components) {
            if (components.rigidBody && Runtime::ValidateRigidBodyComponent(*components.rigidBody).HasError())
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            if (std::ranges::any_of(components.colliders, [](const Runtime::ColliderComponent &component) {
                return Runtime::ValidateColliderComponent(component).HasError();
            }))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            if (std::ranges::any_of(components.physicsConstraints, [](const Runtime::PhysicsConstraintComponent &component) {
                return Runtime::ValidatePhysicsConstraintComponent(component).HasError();
            }))
                return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateComponents(const SceneObjectComponentSet &components) {
            if (Result<void> physics = ValidatePhysicsAuthoringComponents(components); physics.HasError())
                return physics;
            if (components.navigationSurface && Runtime::ValidateNavigationSurfaceComponent(*components.navigationSurface).HasError())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
            if (components.navigationRegion && Runtime::ValidateNavigationRegionComponent(*components.navigationRegion).HasError())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
            if (components.navigationModifier && Runtime::ValidateNavigationModifierComponent(*components.navigationModifier).HasError())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
            if (components.navigationLink && Runtime::ValidateNavigationLinkComponent(*components.navigationLink).HasError())
                return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
            if (components.camera.has_value()) {
                const Runtime::CameraComponent &camera = *components.camera;
                if (!IsValidCameraComponent(camera)) {
                    return Result<void>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidCamera, "Camera authoring values are invalid."));
                }
            }
            if (components.light.has_value()) {
                const Runtime::LightComponent &light = *components.light;
                if (!IsValidLightComponent(light)) {
                    return Result<void>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidLight, "Light authoring values are invalid."));
                }
            }
            if (components.audioSource.has_value() &&
                (Audio::ValidateAudioSoundReference(components.audioSource->sound).HasError() ||
                 Audio::ValidateAudioSoundPlaybackDefaults(components.audioSource->playback).HasError())) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidAudioSource, "Audio source values are invalid."));
            }
            std::vector<Gameplay::BehaviorInstanceId> behaviorIds;
            behaviorIds.reserve(components.behaviors.size());
            for (const Gameplay::BehaviorComponent &behavior : components.behaviors) {
                if (Gameplay::ValidateBehaviorComponent(behavior).HasError() ||
                    std::ranges::find(behaviorIds, behavior.instanceId) != behaviorIds.end()) {
                    return Result<void>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment payload is invalid."));
                }
                behaviorIds.push_back(behavior.instanceId);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSceneNavigationComponents(
            const std::span<const SceneObjectSnapshot> objects,
            const std::optional<std::pair<SceneObjectId, const SceneObjectComponentSet *>> replacement = std::nullopt,
            const SceneObjectComponentSet *appended = nullptr) {
            std::vector<Runtime::NavigationSceneComponentView> views;
            views.reserve(objects.size() + (appended != nullptr ? 1U : 0U));
            const auto collect = [&](const SceneObjectComponentSet &components) {
                views.push_back({.surface = components.navigationSurface ? &*components.navigationSurface : nullptr,
                                 .region = components.navigationRegion ? &*components.navigationRegion : nullptr,
                                 .modifier = components.navigationModifier ? &*components.navigationModifier : nullptr,
                                 .link = components.navigationLink ? &*components.navigationLink : nullptr});
            };
            for (const SceneObjectSnapshot &object : objects) {
                collect(replacement && replacement->first == object.id ? *replacement->second : object.components);
            }
            if (appended != nullptr)
                collect(*appended);
            return Runtime::ValidateNavigationSceneComponentViews(views);
        }

        void ObserveNavigationComponentIds(const SceneObjectComponentSet &components, std::uint64_t &nextSurfaceId,
                                           std::uint64_t &nextRegionId, std::uint64_t &nextModifierId, std::uint64_t &nextLinkId) noexcept {
            const auto advance = [](const std::uint64_t observed, std::uint64_t &next) {
                if (next != 0 && observed >= next)
                    next = observed == std::numeric_limits<std::uint64_t>::max() ? 0 : observed + 1;
            };
            if (components.navigationSurface)
                advance(components.navigationSurface->id.Value(), nextSurfaceId);
            if (components.navigationRegion)
                advance(components.navigationRegion->id.Value(), nextRegionId);
            if (components.navigationModifier)
                advance(components.navigationModifier->id.Value(), nextModifierId);
            if (components.navigationLink)
                advance(components.navigationLink->id.Value(), nextLinkId);
        }

        /** @brief Retargets navigation references that point to a duplicated object's local surface. */
        void RetargetLocalNavigationSurface(SceneObjectComponentSet &components, const Navigation::SurfaceId sourceSurface,
                                            const Navigation::SurfaceId duplicatedSurface) {
            if (components.navigationRegion && components.navigationRegion->surface == sourceSurface)
                components.navigationRegion->surface = duplicatedSurface;
            if (components.navigationModifier && components.navigationModifier->surface == sourceSurface)
                components.navigationModifier->surface = duplicatedSurface;
            if (!components.navigationLink)
                return;
            if (components.navigationLink->start.surface == sourceSurface)
                components.navigationLink->start.surface = duplicatedSurface;
            if (components.navigationLink->end.surface == sourceSurface)
                components.navigationLink->end.surface = duplicatedSurface;
        }

        /** @brief Assigns fresh navigation identities and retargets references local to a duplicated object. */
        [[nodiscard]] Result<void> RegenerateDuplicatedNavigationIdentities(SceneObjectComponentSet &components,
                                                                            const std::uint64_t nextSurfaceId,
                                                                            const std::uint64_t nextRegionId,
                                                                            const std::uint64_t nextModifierId,
                                                                            const std::uint64_t nextLinkId) {
            if (components.navigationSurface) {
                if (nextSurfaceId == 0)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
                const Navigation::SurfaceId sourceSurface = components.navigationSurface->id;
                components.navigationSurface->id = Navigation::SurfaceId::Create(nextSurfaceId).Value();
                RetargetLocalNavigationSurface(components, sourceSurface, components.navigationSurface->id);
            }
            if (components.navigationRegion) {
                if (nextRegionId == 0)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
                components.navigationRegion->id = Navigation::NavigationRegionId::Create(nextRegionId).Value();
            }
            if (components.navigationModifier) {
                if (nextModifierId == 0)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
                components.navigationModifier->id = Navigation::NavigationModifierId::Create(nextModifierId).Value();
            }
            if (components.navigationLink) {
                if (nextLinkId == 0)
                    return Result<void>::Failure(MakeError(Navigation::NavigationErrors::SceneComponentInvalid));
                components.navigationLink->id = Navigation::NavigationLinkId::Create(nextLinkId).Value();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::size_t EstimateBehaviorMemoryBytes(const std::vector<Gameplay::BehaviorComponent> &behaviors) noexcept {
            std::size_t total = behaviors.size() * sizeof(Gameplay::BehaviorComponent);
            for (const Gameplay::BehaviorComponent &behavior : behaviors) {
                total += behavior.typeId.Value().size();
                for (const Gameplay::BehaviorField &field : behavior.fields) {
                    total += field.name.size();
                    if (const auto *text = std::get_if<std::string>(&field.value))
                        total += text->size();
                }
            }
            return total;
        }

        [[nodiscard]] std::size_t EstimateNavigationComponentMemoryBytes(const SceneObjectComponentSet &components) noexcept {
            const std::size_t surfaceProfiles =
                components.navigationSurface ? components.navigationSurface->profiles.size() * sizeof(Navigation::NavigationAgentProfileId)
                                             : 0U;
            const std::size_t linkProfiles =
                components.navigationLink ? components.navigationLink->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
            return surfaceProfiles + linkProfiles;
        }

        template <typename Delta> [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const Delta &) noexcept {
            return sizeof(Delta);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const CreatedObjectDelta &delta) noexcept {
            return sizeof(delta) + delta.object.name.size() + EstimateBehaviorMemoryBytes(delta.object.components.behaviors) +
                   EstimateNavigationComponentMemoryBytes(delta.object.components);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const RenamedObjectDelta &delta) noexcept {
            return sizeof(delta) + delta.before.size() + delta.after.size();
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const DeletedObjectsDelta &delta) noexcept {
            std::size_t bytes = sizeof(delta) + delta.objects.size() * sizeof(IndexedSceneObject);
            for (const IndexedSceneObject &object : delta.objects) {
                bytes += object.object.name.size() + EstimateBehaviorMemoryBytes(object.object.components.behaviors) +
                         EstimateNavigationComponentMemoryBytes(object.object.components) +
                         object.object.components.colliders.size() * sizeof(Runtime::ColliderComponent) +
                         object.object.components.physicsConstraints.size() * sizeof(Runtime::PhysicsConstraintComponent);
                for (const Runtime::ColliderComponent &collider : object.object.components.colliders)
                    bytes += collider.materials.size() * sizeof(Runtime::PhysicsColliderMaterialBinding);
            }
            return bytes + delta.prefabInstances.size() * sizeof(IndexedPrefabInstance);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const DeletedPrefabInstancesDelta &delta) noexcept {
            return sizeof(delta) + delta.instances.size() * sizeof(IndexedPrefabInstance);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const TransformedObjectsDelta &delta) noexcept {
            return sizeof(delta) + delta.objects.size() * sizeof(TransformedObjectDelta);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const BehaviorsChangedDelta &delta) noexcept {
            return sizeof(delta) + EstimateBehaviorMemoryBytes(delta.before) + EstimateBehaviorMemoryBytes(delta.after);
        }

        [[nodiscard]] std::size_t EstimateTypedDeltaMemoryBytes(const NavigationComponentsChangedDelta &delta) noexcept {
            const auto profileBytes = [](const std::optional<Runtime::NavigationSurfaceComponent> &surface) {
                return surface ? surface->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
            };
            const auto linkProfileBytes = [](const std::optional<Runtime::NavigationLinkComponent> &link) {
                return link ? link->profiles.size() * sizeof(Navigation::NavigationAgentProfileId) : 0U;
            };
            return sizeof(delta) + profileBytes(delta.surfaceBefore) + profileBytes(delta.surfaceAfter) +
                   linkProfileBytes(delta.linkBefore) + linkProfileBytes(delta.linkAfter);
        }

        [[nodiscard]] std::size_t EstimateMemoryBytes(const SceneCommandDelta &delta, const std::size_t affectedObjectCount) noexcept {
            return affectedObjectCount * sizeof(SceneObjectId) + std::visit([]<typename Delta>(const Delta &typedDelta) {
                return EstimateTypedDeltaMemoryBytes(typedDelta);
            }, delta);
        }

        [[nodiscard]] SceneObjectId DeltaRootObject(const SceneCommandDelta &delta) noexcept {
            return std::visit([]<typename Delta>(const Delta &typedDelta) {
                if constexpr (std::is_same_v<Delta, CreatedObjectDelta>) {
                    return typedDelta.object.id;
                } else if constexpr (std::is_same_v<Delta, DeletedObjectsDelta>) {
                    return typedDelta.roots.empty() ? SceneObjectId{} : typedDelta.roots.front();
                } else if constexpr (std::is_same_v<Delta, TransformedObjectsDelta>) {
                    return typedDelta.objects.empty() ? SceneObjectId{} : typedDelta.objects.front().object;
                } else if constexpr (std::is_same_v<Delta, CreatedPrefabInstanceDelta> ||
                                     std::is_same_v<Delta, PrefabInstanceTransformDelta> ||
                                     std::is_same_v<Delta, PrefabInstanceReparentDelta> ||
                                     std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                    return SceneObjectId{};
                } else {
                    return typedDelta.object;
                }
            }, delta);
        }

        [[nodiscard]] std::optional<Prefab::PrefabInstanceId> DeltaRootPrefabInstance(const SceneCommandDelta &delta) noexcept {
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

        void AddComponent(SceneObjectComponentSet &components, const ComponentType type) {
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

        [[nodiscard]] bool HasComponent(const SceneObjectComponentSet &components, const ComponentType type) noexcept {
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

        void RemoveComponent(SceneObjectComponentSet &components, const ComponentType type) {
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

        void RestoreComponent(SceneObjectComponentSet &components, const ComponentRemovedDelta &delta) {
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

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CreatedObjectDelta &delta) {
            const std::size_t index = std::min(delta.index, objects.size());
            objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), delta.object);
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const RenamedObjectDelta &delta) {
            FindObject(objects, delta.object)->name = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectDelta &delta) {
            FindObject(objects, delta.object)->localTransform = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectsDelta &delta) {
            for (const TransformedObjectDelta &object : delta.objects)
                FindObject(objects, object.object)->localTransform = object.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CameraChangedDelta &delta) {
            FindObject(objects, delta.object)->components.camera = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const LightChangedDelta &delta) {
            FindObject(objects, delta.object)->components.light = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TriggerVolumeChangedDelta &delta) {
            FindObject(objects, delta.object)->components.triggerVolume = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const AudioSourceChangedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                object->components.audioSource = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const NavigationComponentsChangedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end()) {
                object->components.navigationSurface = delta.surfaceAfter;
                object->components.navigationRegion = delta.regionAfter;
                object->components.navigationModifier = delta.modifierAfter;
                object->components.navigationLink = delta.linkAfter;
            }
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const EditorStateChangedDelta &delta) {
            FindObject(objects, delta.object)->editorState = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const BehaviorsChangedDelta &delta) {
            FindObject(objects, delta.object)->components.behaviors = delta.after;
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentAddedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                AddComponent(object->components, delta.type);
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentRemovedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                RemoveComponent(object->components, delta.type);
        }

        void ApplyTypedDelta(std::vector<SceneObjectSnapshot> &objects, const DeletedObjectsDelta &delta) {
            std::erase_if(objects, [&delta](const SceneObjectSnapshot &object) {
                return std::ranges::any_of(delta.objects, [&object](const IndexedSceneObject &removed) {
                    return removed.object.id == object.id;
                });
            });
        }

        void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
            const std::size_t index = std::min(delta.index, instances.size());
            instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(index), delta.instance);
        }

        void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
            FindPrefabInstance(instances, delta.instance)->rootTransform = delta.after;
        }

        void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceReparentDelta &delta) {
            FindPrefabInstance(instances, delta.instance)->parent = delta.after;
        }

        void RemovePrefabInstances(std::vector<ScenePrefabInstance> &instances,
                                   const std::vector<IndexedPrefabInstance> &removedInstances) {
            std::erase_if(instances, [&removedInstances](const ScenePrefabInstance &instance) {
                return std::ranges::any_of(removedInstances, [&instance](const IndexedPrefabInstance &removed) {
                    return removed.instance.instanceId == instance.instanceId;
                });
            });
        }

        void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedPrefabInstancesDelta &delta) {
            RemovePrefabInstances(instances, delta.instances);
        }

        void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedObjectsDelta &delta) {
            RemovePrefabInstances(instances, delta.prefabInstances);
        }

        void ApplyDelta(std::vector<SceneObjectSnapshot> &objects, const SceneCommandDelta &delta) {
            std::visit([&objects]<typename Delta>(const Delta &typedDelta) {
                if constexpr (!std::is_same_v<Delta, CreatedPrefabInstanceDelta> && !std::is_same_v<Delta, PrefabInstanceTransformDelta> &&
                              !std::is_same_v<Delta, PrefabInstanceReparentDelta> && !std::is_same_v<Delta, DeletedPrefabInstancesDelta>) {
                    ApplyTypedDelta(objects, typedDelta);
                }
            }, delta);
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CreatedObjectDelta &delta) {
            std::erase_if(objects, [&delta](const SceneObjectSnapshot &object) {
                return object.id == delta.object.id;
            });
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const RenamedObjectDelta &delta) {
            FindObject(objects, delta.object)->name = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectDelta &delta) {
            FindObject(objects, delta.object)->localTransform = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TransformedObjectsDelta &delta) {
            for (const TransformedObjectDelta &object : delta.objects)
                FindObject(objects, object.object)->localTransform = object.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const CameraChangedDelta &delta) {
            FindObject(objects, delta.object)->components.camera = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const LightChangedDelta &delta) {
            FindObject(objects, delta.object)->components.light = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const TriggerVolumeChangedDelta &delta) {
            FindObject(objects, delta.object)->components.triggerVolume = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const AudioSourceChangedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                object->components.audioSource = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const NavigationComponentsChangedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end()) {
                object->components.navigationSurface = delta.surfaceBefore;
                object->components.navigationRegion = delta.regionBefore;
                object->components.navigationModifier = delta.modifierBefore;
                object->components.navigationLink = delta.linkBefore;
            }
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const EditorStateChangedDelta &delta) {
            FindObject(objects, delta.object)->editorState = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const BehaviorsChangedDelta &delta) {
            FindObject(objects, delta.object)->components.behaviors = delta.before;
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentAddedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                RemoveComponent(object->components, delta.type);
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const ComponentRemovedDelta &delta) {
            if (const auto object = FindObject(objects, delta.object); object != objects.end())
                RestoreComponent(object->components, delta);
        }

        void RevertTypedDelta(std::vector<SceneObjectSnapshot> &objects, const DeletedObjectsDelta &delta) {
            for (const IndexedSceneObject &removed : delta.objects) {
                const std::size_t index = std::min(removed.index, objects.size());
                objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(index), removed.object);
            }
        }

        void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
            std::erase_if(instances, [&delta](const ScenePrefabInstance &instance) {
                return instance.instanceId == delta.instance.instanceId;
            });
        }

        void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
            FindPrefabInstance(instances, delta.instance)->rootTransform = delta.before;
        }

        void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceReparentDelta &delta) {
            FindPrefabInstance(instances, delta.instance)->parent = delta.before;
        }

        void RestorePrefabInstances(std::vector<ScenePrefabInstance> &instances,
                                    const std::vector<IndexedPrefabInstance> &removedInstances) {
            for (const IndexedPrefabInstance &removed : removedInstances) {
                const std::size_t index = std::min(removed.index, instances.size());
                instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(index), removed.instance);
            }
        }

        void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedPrefabInstancesDelta &delta) {
            RestorePrefabInstances(instances, delta.instances);
        }

        void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const DeletedObjectsDelta &delta) {
            RestorePrefabInstances(instances, delta.prefabInstances);
        }

        template <bool Revert>
        void MutateDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
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

        void ApplyDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
                        const SceneCommandDelta &delta) {
            MutateDelta<false>(objects, instances, delta);
        }

        void RevertDelta(std::vector<SceneObjectSnapshot> &objects, std::vector<ScenePrefabInstance> &instances,
                         const SceneCommandDelta &delta) {
            MutateDelta<true>(objects, instances, delta);
        }

        [[nodiscard]] Result<void> ValidateLoadedBehaviors(const SceneObjectSnapshot &object,
                                                           std::unordered_set<std::uint64_t> &behaviorIds,
                                                           std::uint64_t &maximumBehaviorId) {
            for (const Gameplay::BehaviorComponent &behavior : object.components.behaviors) {
                if (!behaviorIds.insert(behavior.instanceId.value).second) {
                    return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidBehavior,
                                                                   "Loaded behavior instance IDs must be unique across the scene."));
                }
                maximumBehaviorId = std::max(maximumBehaviorId, behavior.instanceId.value);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoadedObject(const SceneObjectSnapshot &object, std::unordered_set<std::uint64_t> &objectIds,
                                                        std::unordered_set<std::uint64_t> &behaviorIds, std::uint64_t &maximumObjectId,
                                                        std::uint64_t &maximumBehaviorId) {
            if (!object.id.IsValid() || !objectIds.insert(object.id.value).second) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Loaded scene object IDs must be non-zero and unique."));
            }
            if (!IsValidSceneObjectName(object.name)) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidName, "Loaded scene object names must contain 1 to 128 bytes."));
            }
            if (!IsValid(object.localTransform)) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Loaded scene object transforms must be finite."));
            }
            if (const Result<void> primitive = ValidateDescriptor(object.primitiveMesh); primitive.HasError())
                return primitive;
            if ((object.meshAsset.has_value() && !object.meshAsset->IsValid()) ||
                (object.meshAsset.has_value() && object.primitiveMesh.has_value())) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata,
                                      "Loaded scene object mesh references must be valid and mutually exclusive."));
            }
            if (const Result<void> components = ValidateComponents(object.components); components.HasError())
                return components;
            if (Result<void> behaviors = ValidateLoadedBehaviors(object, behaviorIds, maximumBehaviorId); behaviors.HasError())
                return behaviors;
            maximumObjectId = std::max(maximumObjectId, object.id.value);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoadedObjectHierarchy(const std::vector<SceneObjectSnapshot> &objects,
                                                                 const std::unordered_set<std::uint64_t> &objectIds,
                                                                 const SceneObjectSnapshot &object) {
            if (!object.parent.has_value())
                return Result<void>::Success();
            if (*object.parent == object.id || !objectIds.contains(object.parent->value)) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::ParentNotFound,
                                      "Loaded scene object parents must reference a different object in the same scene."));
            }
            std::unordered_set<std::uint64_t> ancestors;
            std::optional<SceneObjectId> ancestor = object.parent;
            while (ancestor.has_value()) {
                if (!ancestors.insert(ancestor->value).second) {
                    return Result<void>::Failure(
                        MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Loaded scene object hierarchy must not contain a cycle."));
                }
                const auto parent = FindObject(objects, *ancestor);
                if (parent == objects.end())
                    break;
                ancestor = parent->parent;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLoadedHierarchy(const std::vector<SceneObjectSnapshot> &objects,
                                                           const std::unordered_set<std::uint64_t> &objectIds) {
            for (const SceneObjectSnapshot &object : objects) {
                if (Result<void> valid = ValidateLoadedObjectHierarchy(objects, objectIds, object); valid.HasError())
                    return valid;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint64_t> ValidateLoadedPrefabInstances(const std::vector<ScenePrefabInstance> &instances,
                                                                          const std::unordered_set<std::uint64_t> &objectIds) {
            std::unordered_set<std::uint64_t> instanceIds;
            instanceIds.reserve(instances.size());
            std::uint64_t maximumInstanceId = 0;
            for (const ScenePrefabInstance &instance : instances) {
                if (!instance.instanceId.IsValid() || !instance.sourcePrefab.IsValid() || !IsValid(instance.rootTransform) ||
                    !instanceIds.insert(instance.instanceId.Value()).second ||
                    (instance.parent.has_value() && !objectIds.contains(instance.parent->value))) {
                    return Result<std::uint64_t>::Failure(
                        MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance,
                                          "Loaded prefab instances require unique identities, valid asset references, finite root "
                                          "transforms, and containing-scene object parents."));
                }
                maximumInstanceId = std::max(maximumInstanceId, instance.instanceId.Value());
            }
            if (maximumInstanceId == std::numeric_limits<std::uint64_t>::max()) {
                return Result<std::uint64_t>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance,
                                      "Loaded prefab instance IDs must leave space for future authored placements."));
            }
            return Result<std::uint64_t>::Success(maximumInstanceId);
        }

        [[nodiscard]] std::vector<SceneObjectId> SelectExistingObjects(const std::vector<SceneObjectSnapshot> &objects,
                                                                       const std::span<const SceneObjectId> requested) {
            std::vector<SceneObjectId> selected;
            selected.reserve(requested.size());
            for (const SceneObjectId object : requested) {
                if (!object.IsValid() || FindObject(objects, object) == objects.end() ||
                    std::ranges::find(selected, object) != selected.end())
                    continue;
                selected.push_back(object);
            }
            return selected;
        }

        [[nodiscard]] std::unordered_set<std::uint64_t> ObjectIdSet(const std::span<const SceneObjectId> objects) {
            std::unordered_set<std::uint64_t> ids;
            ids.reserve(objects.size());
            for (const SceneObjectId object : objects)
                ids.insert(object.value);
            return ids;
        }

        [[nodiscard]] bool HasSelectedAncestor(const std::vector<SceneObjectSnapshot> &objects,
                                               const std::unordered_set<std::uint64_t> &selectedIds, const SceneObjectId object) {
            auto current = FindObject(objects, object);
            std::optional<SceneObjectId> ancestor = current->parent;
            while (ancestor.has_value()) {
                if (selectedIds.contains(ancestor->value))
                    return true;
                current = FindObject(objects, *ancestor);
                ancestor = current == objects.end() ? std::nullopt : current->parent;
            }
            return false;
        }

        [[nodiscard]] std::vector<SceneObjectId> DeletionRoots(const std::vector<SceneObjectSnapshot> &objects,
                                                               const std::span<const SceneObjectId> selected) {
            const std::unordered_set<std::uint64_t> selectedIds = ObjectIdSet(selected);
            std::vector<SceneObjectId> roots;
            roots.reserve(selected.size());
            for (const SceneObjectId object : selected) {
                if (!HasSelectedAncestor(objects, selectedIds, object))
                    roots.push_back(object);
            }
            return roots;
        }

        [[nodiscard]] std::vector<SceneObjectId> CollectRemovedObjects(const std::vector<SceneObjectSnapshot> &objects,
                                                                       const std::span<const SceneObjectId> roots,
                                                                       std::unordered_set<std::uint64_t> &removedIds) {
            std::vector<SceneObjectId> removed{roots.begin(), roots.end()};
            std::deque<SceneObjectId> pending{roots.begin(), roots.end()};
            removedIds.reserve(objects.size());
            for (const SceneObjectId root : roots)
                removedIds.insert(root.value);
            while (!pending.empty()) {
                const SceneObjectId parent = pending.front();
                pending.pop_front();
                for (const SceneObjectSnapshot &candidate : objects) {
                    if (candidate.parent != parent || !removedIds.insert(candidate.id.value).second)
                        continue;
                    removed.push_back(candidate.id);
                    pending.push_back(candidate.id);
                }
            }
            return removed;
        }

        [[nodiscard]] DeletedObjectsDelta CaptureDeletedObjects(const std::vector<SceneObjectSnapshot> &objects,
                                                                const std::vector<ScenePrefabInstance> &prefabInstances,
                                                                std::vector<SceneObjectId> roots,
                                                                const std::unordered_set<std::uint64_t> &removedIds) {
            DeletedObjectsDelta deleted{.roots = std::move(roots)};
            deleted.objects.reserve(removedIds.size());
            std::size_t index = 0;
            for (const SceneObjectSnapshot &object : objects) {
                if (removedIds.contains(object.id.value))
                    deleted.objects.emplace_back(object, index);
                ++index;
            }
            index = 0;
            for (const ScenePrefabInstance &instance : prefabInstances) {
                if (instance.parent.has_value() && removedIds.contains(instance.parent->value))
                    deleted.prefabInstances.emplace_back(instance, index);
                ++index;
            }
            return deleted;
        }

        [[nodiscard]] std::optional<std::string> UniqueSiblingName(const std::string_view baseName,
                                                                   const std::optional<SceneObjectId> parent,
                                                                   const std::span<const SceneObjectSnapshot> objects) {
            const auto nameAvailable = [parent, objects](const std::string_view candidate) {
                return std::ranges::none_of(objects, [parent, candidate](const SceneObjectSnapshot &object) {
                    return object.parent == parent && object.name == candidate;
                });
            };
            if (nameAvailable(baseName))
                return std::string{baseName};
            for (std::uint64_t suffix = 2; suffix <= std::numeric_limits<std::uint32_t>::max(); ++suffix) {
                const std::string suffixText = std::format(" {}", suffix);
                const std::size_t prefixLength = MaximumSceneObjectNameBytes - suffixText.size();
                std::string candidate = std::format("{}{}", baseName.substr(0, prefixLength), suffixText);
                if (nameAvailable(candidate))
                    return candidate;
            }
            return std::nullopt;
        }
    }  // namespace

    struct SceneDocumentCommandExecutor::PrefabCommitContext final {
        SceneCommandDelta delta;
        Prefab::PrefabInstanceId instance;
        DocumentChangeKind kind;
        bool advanceInstanceId{};
    };

    struct SceneDocumentCommandExecutor::ObjectCommitContext final {
        SceneCommandDelta delta;
        SceneObjectId object;
        DocumentChangeKind kind;
    };

    struct EditorHistory::Impl {
        Impl() {
            undo.reserve(kMaximumHistoryEntries);
            redo.reserve(kMaximumHistoryEntries);
        }

        std::vector<HistoryRecord> undo;
        std::vector<HistoryRecord> redo;
        std::size_t memoryBytes{0};
    };

    namespace {
        template <typename History> void ClearRedo(History &history) noexcept {
            for (const HistoryRecord &entry : history.redo) {
                history.memoryBytes -= entry.memoryBytes;
            }
            history.redo.clear();
        }

        template <typename History> void PushHistory(History &history, HistoryRecord entry) {
            ClearRedo(history);
            history.memoryBytes += entry.memoryBytes;
            history.undo.push_back(std::move(entry));
            while (history.undo.size() > kMaximumHistoryEntries || history.memoryBytes > kMaximumHistoryBytes) {
                history.memoryBytes -= history.undo.front().memoryBytes;
                history.undo.erase(history.undo.begin());
            }
        }

        [[nodiscard]] Result<void> ValidateHistoryDelta(const SceneCommandDelta &delta, const std::size_t affectedObjectCount) {
            if (EstimateMemoryBytes(delta, affectedObjectCount) > kMaximumHistoryBytes) {
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::HistoryEntryTooLarge,
                                                               "Scene command exceeds the semantic history memory budget."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePrefabParent(const std::vector<SceneObjectSnapshot> &objects,
                                                        const std::optional<SceneObjectId> parent) {
            if (parent.has_value() && FindObject(objects, *parent) == objects.end()) {
                return Result<void>::Failure(
                    MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Prefab root parent must be a containing-scene object."));
            }
            if (parent.has_value() && IsEffectivelyLocked(objects, *parent))
                return Result<void>::Failure(LockedObjectError());
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ScenePrefabInstance *> FindEditablePrefabInstance(const std::vector<SceneObjectSnapshot> &objects,
                                                                               std::vector<ScenePrefabInstance> &instances,
                                                                               const Prefab::PrefabInstanceId id) {
            const auto instance = FindPrefabInstance(instances, id);
            if (instance == instances.end()) {
                return Result<ScenePrefabInstance *>::Failure(
                    MakeDocumentError(SceneDocumentErrors::PrefabInstanceNotFound, "Prefab instance does not exist."));
            }
            if (IsPrefabInstanceLocked(objects, *instance))
                return Result<ScenePrefabInstance *>::Failure(LockedObjectError());
            return Result<ScenePrefabInstance *>::Success(std::to_address(instance));
        }

        [[nodiscard]] Result<Prefab::PrefabInstanceId> AllocatePrefabInstanceId(const std::uint64_t nextInstanceId) {
            if (nextInstanceId == std::numeric_limits<std::uint64_t>::max()) {
                return Result<Prefab::PrefabInstanceId>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance, "Prefab instance identity space is exhausted."));
            }
            return Prefab::PrefabInstanceId::Create(nextInstanceId);
        }

        [[nodiscard]] SceneCommandResult PrefabNoOpResult(const DocumentRevision revision, const DocumentStateId state,
                                                          const Prefab::PrefabInstanceId instance, const DocumentChangeKind kind) {
            SceneCommandResult result{{}, revision, state, kind, {}, false};
            result.prefabInstance = instance;
            return result;
        }

        [[nodiscard]] SceneCommandResult HistoryCommandResult(const SceneDocument &document, const HistoryRecord &entry,
                                                              const DocumentChangeKind kind) {
            SceneCommandResult result{DeltaRootObject(entry.delta),
                                      document.Revision(),
                                      document.State(),
                                      kind,
                                      entry.affectedObjects,
                                      true};
            result.prefabInstance = DeltaRootPrefabInstance(entry.delta);
            result.affectedPrefabInstances = entry.affectedPrefabInstances;
            return result;
        }
    }  // namespace

    /** @copydoc EditorHistory::EditorHistory */
    EditorHistory::EditorHistory() : m_impl(std::make_unique<Impl>()) {}

    /** @copydoc EditorHistory::~EditorHistory */
    EditorHistory::~EditorHistory() = default;

    /** @copydoc EditorHistory::CanUndo */
    bool EditorHistory::CanUndo() const noexcept {
        return !m_impl->undo.empty();
    }

    /** @copydoc EditorHistory::CanRedo */
    bool EditorHistory::CanRedo() const noexcept {
        return !m_impl->redo.empty();
    }

    /** @copydoc EditorHistory::Clear */
    void EditorHistory::Clear() noexcept {
        m_impl->undo.clear();
        m_impl->redo.clear();
        m_impl->memoryBytes = 0;
    }

    /** @copydoc SceneDocument::Revision */
    DocumentRevision SceneDocument::Revision() const noexcept {
        return m_revision;
    }

    /** @copydoc SceneDocument::State */
    DocumentStateId SceneDocument::State() const noexcept {
        return m_state;
    }

    /** @copydoc SceneDocument::SavedRevision */
    DocumentRevision SceneDocument::SavedRevision() const noexcept {
        return m_savedRevision;
    }

    /** @copydoc SceneDocument::SavedState */
    DocumentStateId SceneDocument::SavedState() const noexcept {
        return m_savedState;
    }

    /** @copydoc SceneDocument::IsDirty */
    bool SceneDocument::IsDirty() const noexcept {
        return m_state != m_savedState;
    }

    /** @copydoc SceneDocument::Snapshot */
    SceneDocumentSnapshot SceneDocument::Snapshot() const {
        return SceneDocumentSnapshot{m_revision, m_state, m_objects, m_prefabInstances};
    }

    /** @copydoc SceneDocument::Objects */
    std::span<const SceneObjectSnapshot> SceneDocument::Objects() const noexcept {
        return m_objects;
    }

    /** @copydoc SceneDocument::PrefabInstances */
    std::span<const ScenePrefabInstance> SceneDocument::PrefabInstances() const noexcept {
        return m_prefabInstances;
    }

    /** @copydoc SceneDocument::Contains */
    bool SceneDocument::Contains(const SceneObjectId object) const noexcept {
        return FindObject(m_objects, object) != m_objects.end();
    }

    /** @copydoc SceneDocument::MarkSaved */
    Result<void> SceneDocument::MarkSaved(const DocumentRevision revision, const DocumentStateId state) {
        if (revision > m_revision || !state.IsValid() || state.value >= m_nextStateId) {
            return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidSavedState,
                                                           "Saved revision and state must belong to this document session."));
        }
        m_savedRevision = revision;
        m_savedState = state;
        return Result<void>::Success();
    }

    /** @copydoc SceneDocument::LoadSaved */
    Result<void> SceneDocument::LoadSaved(std::vector<SceneObjectSnapshot> objects, std::vector<ScenePrefabInstance> prefabInstances) {
        std::unordered_set<std::uint64_t> objectIds;
        std::unordered_set<std::uint64_t> behaviorIds;
        objectIds.reserve(objects.size());
        std::uint64_t maximumObjectId = 0;
        std::uint64_t maximumBehaviorId = 0;
        std::uint64_t nextNavigationSurfaceId = 1;
        std::uint64_t nextNavigationRegionId = 1;
        std::uint64_t nextNavigationModifierId = 1;
        std::uint64_t nextNavigationLinkId = 1;
        for (const SceneObjectSnapshot &object : objects) {
            if (Result<void> valid = ValidateLoadedObject(object, objectIds, behaviorIds, maximumObjectId, maximumBehaviorId);
                valid.HasError())
                return valid;
            ObserveNavigationComponentIds(object.components, nextNavigationSurfaceId, nextNavigationRegionId, nextNavigationModifierId,
                                          nextNavigationLinkId);
        }
        if (maximumObjectId == std::numeric_limits<std::uint64_t>::max() ||
            maximumBehaviorId == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::ObjectNotFound,
                                                           "Loaded scene object IDs must leave space for future authored objects."));
        }

        if (Result<void> validHierarchy = ValidateLoadedHierarchy(objects, objectIds); validHierarchy.HasError())
            return validHierarchy;
        if (Result<void> navigation = ValidateSceneNavigationComponents(objects); navigation.HasError())
            return navigation;
        auto maximumPrefabInstanceId = ValidateLoadedPrefabInstances(prefabInstances, objectIds);
        if (maximumPrefabInstanceId.HasError())
            return Result<void>::Failure(maximumPrefabInstanceId.ErrorValue());

        m_objects = std::move(objects);
        m_prefabInstances = std::move(prefabInstances);
        m_revision = {};
        m_savedRevision = {};
        m_state = DocumentStateId{1};
        m_savedState = m_state;
        m_nextStateId = 2;
        m_nextNavigationSurfaceId = nextNavigationSurfaceId;
        m_nextNavigationRegionId = nextNavigationRegionId;
        m_nextNavigationModifierId = nextNavigationModifierId;
        m_nextNavigationLinkId = nextNavigationLinkId;
        m_nextObjectId = maximumObjectId + 1;
        m_nextBehaviorInstanceId = maximumBehaviorId + 1;
        m_nextPrefabInstanceId = maximumPrefabInstanceId.Value() + 1;
        return Result<void>::Success();
    }

    /** @copydoc SceneDocument::LoadRecovered */
    Result<void> SceneDocument::LoadRecovered(std::vector<SceneObjectSnapshot> objects, std::vector<ScenePrefabInstance> prefabInstances) {
        if (Result<void> loaded = LoadSaved(std::move(objects), std::move(prefabInstances)); loaded.HasError())
            return loaded;
        m_revision = DocumentRevision{1};
        m_state = DocumentStateId{2};
        m_nextStateId = 3;
        return Result<void>::Success();
    }

    /** @copydoc SceneDocumentCommandExecutor::SceneDocumentCommandExecutor */
    SceneDocumentCommandExecutor::SceneDocumentCommandExecutor(SceneDocument &document, EditorHistory &history) noexcept
        : m_document(document), m_history(history) {}

    /** @copydoc SceneDocumentCommandExecutor::CommitObject */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitObject(ObjectCommitContext context) {
        const std::size_t memoryBytes = EstimateMemoryBytes(context.delta, 1);
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, context.delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        std::vector affected{context.object};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(context.delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(
            {context.object, m_document.m_revision, m_document.m_state, context.kind, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::CommitPrefab */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitPrefab(PrefabCommitContext context) {
        if (const Result<void> validHistory = ValidateHistoryDelta(context.delta, 1); validHistory.HasError())
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());

        const std::size_t memoryBytes = EstimateMemoryBytes(context.delta, 1);
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, context.delta);
        if (context.advanceInstanceId)
            ++m_document.m_nextPrefabInstanceId;
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};

        std::vector affected{context.instance};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(context.delta), {}, memoryBytes, affected});
        SceneCommandResult result{{}, m_document.m_revision, m_document.m_state, context.kind, {}, true};
        result.prefabInstance = context.instance;
        result.affectedPrefabInstances = std::move(affected);
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const CreateSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const CreateSceneObjectCommand &command) {
        if (!IsValidSceneObjectName(command.name)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
        }
        if (!IsValid(command.localTransform)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Scene object transform must be finite."));
        }
        if (const Result<void> descriptorResult = ValidateDescriptor(command.primitiveMesh); descriptorResult.HasError()) {
            return Result<SceneCommandResult>::Failure(descriptorResult.ErrorValue());
        }
        if ((command.meshAsset.has_value() && !command.meshAsset->IsValid()) ||
            (command.meshAsset.has_value() && command.primitiveMesh.has_value())) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata,
                                  "Scene object mesh references must be valid and mutually exclusive."));
        }
        if (const Result<void> componentResult = ValidateComponents(command.components); componentResult.HasError()) {
            return Result<SceneCommandResult>::Failure(componentResult.ErrorValue());
        }
        if (const Result<void> navigation = ValidateSceneNavigationComponents(m_document.m_objects, std::nullopt, &command.components);
            navigation.HasError()) {
            return Result<SceneCommandResult>::Failure(navigation.ErrorValue());
        }
        if (command.parent.has_value() && FindObject(m_document.m_objects, *command.parent) == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Scene object parent does not exist."));
        }
        if (command.parent.has_value() && IsEffectivelyLocked(m_document.m_objects, *command.parent))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        const SceneObjectId id{m_document.m_nextObjectId};
        SceneCommandDelta delta = CreatedObjectDelta{
            .object = SceneObjectSnapshot{.id = id,
                                          .parent = command.parent,
                                          .name = command.name,
                                          .localTransform = command.localTransform,
                                          .primitiveMesh = command.primitiveMesh,
                                          .components = command.components,
                                          .meshAsset = command.meshAsset},
            .index = m_document.m_objects.size(),
            .kind = DocumentChangeKind::Created,
        };
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, 1); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        ObserveNavigationComponentIds(command.components, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                      m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
        ++m_document.m_nextObjectId;
        return CommitObject({std::move(delta), id, DocumentChangeKind::Created});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RenameSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RenameSceneObjectCommand &command) {
        if (!IsValidSceneObjectName(command.name)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
        }
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (object->name == command.name) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::Renamed, {}, false});
        }

        SceneCommandDelta delta = RenamedObjectDelta{object->id, object->name, command.name};
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, 1);
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, 1); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        std::vector affected{object->id};
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(SceneCommandResult{command.object, m_document.m_revision, m_document.m_state,
                                                                      DocumentChangeKind::Renamed, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformCommand &command) {
        return Execute(SetSceneObjectTransformsCommand{{
            SceneObjectTransformUpdate{.object = command.object, .localTransform = command.localTransform},
        }});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformsCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTransformsCommand &command) {
        if (command.updates.empty()) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{{}, m_document.m_revision, m_document.m_state, DocumentChangeKind::TransformChanged, {}, false});
        }

        std::vector<TransformedObjectDelta> changed;
        changed.reserve(command.updates.size());
        std::vector<SceneObjectId> seen;
        seen.reserve(command.updates.size());
        for (const SceneObjectTransformUpdate &update : command.updates) {
            if (!update.object.IsValid() || !IsValid(update.localTransform)) {
                return Result<SceneCommandResult>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Batch transform values must be finite."));
            }
            if (std::ranges::find(seen, update.object) != seen.end()) {
                return Result<SceneCommandResult>::Failure(
                    MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Batch transform object identities must be unique."));
            }
            seen.push_back(update.object);
            const auto object = FindObject(m_document.m_objects, update.object);
            if (object == m_document.m_objects.end()) {
                return Result<SceneCommandResult>::Failure(
                    MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Batch transform object does not exist."));
            }
            if (IsEffectivelyLocked(m_document.m_objects, update.object))
                return Result<SceneCommandResult>::Failure(LockedObjectError());
            if (object->localTransform != update.localTransform) {
                changed.emplace_back(object->id, object->localTransform, update.localTransform);
            }
        }
        if (changed.empty()) {
            return Result<SceneCommandResult>::Success(SceneCommandResult{command.updates.front().object,
                                                                          m_document.m_revision,
                                                                          m_document.m_state,
                                                                          DocumentChangeKind::TransformChanged,
                                                                          {},
                                                                          false});
        }

        SceneCommandDelta delta = TransformedObjectsDelta{std::move(changed)};
        const std::vector<TransformedObjectDelta> &deltaObjects = std::get<TransformedObjectsDelta>(delta).objects;
        std::vector<SceneObjectId> affected;
        affected.reserve(deltaObjects.size());
        for (const TransformedObjectDelta &object : deltaObjects) {
            affected.push_back(object.object);
        }
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, affected.size()); validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, affected.size());
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        const SceneObjectId primary = affected.front();
        PushHistory(*m_history.m_impl, HistoryRecord{beforeState, m_document.m_state, std::move(delta), affected, memoryBytes});
        return Result<SceneCommandResult>::Success(SceneCommandResult{primary, m_document.m_revision, m_document.m_state,
                                                                      DocumentChangeKind::TransformChanged, std::move(affected), true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectCameraCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectCameraCommand &command) {
        if (!IsValidCameraComponent(command.camera)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidCamera, "Camera authoring values are invalid."));
        }
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (!object->components.camera.has_value()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidCamera, "Scene object has no camera component."));
        }
        if (*object->components.camera == command.camera) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        }

        return CommitObject({CameraChangedDelta{object->id, *object->components.camera, command.camera}, command.object,
                             DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectLightCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectLightCommand &command) {
        if (!IsValidLightComponent(command.light)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidLight, "Light authoring values are invalid."));
        }
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (!object->components.light.has_value()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidLight, "Scene object has no light component."));
        }
        if (*object->components.light == command.light) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        }

        return CommitObject({LightChangedDelta{object->id, *object->components.light, command.light}, command.object,
                             DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectTriggerVolumeCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectTriggerVolumeCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (!object->components.triggerVolume.has_value()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTriggerVolume, "Scene object has no trigger volume component."));
        }
        if (*object->components.triggerVolume == command.triggerVolume) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        }

        return CommitObject({TriggerVolumeChangedDelta{object->id, *object->components.triggerVolume, command.triggerVolume},
                             command.object, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectAudioSourceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectAudioSourceCommand &command) {
        if (!IsValidAudioSourceComponent(command.audioSource)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidAudioSource, "Audio source reference or playback values are invalid."));
        }
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (!object->components.audioSource.has_value()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidAudioSource, "Scene object has no audio source component."));
        }
        if (*object->components.audioSource == command.audioSource) {
            return Result<SceneCommandResult>::Success(
                SceneCommandResult{object->id, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        }

        return CommitObject({AudioSourceChangedDelta{object->id, *object->components.audioSource, command.audioSource}, command.object,
                             DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::CommitNavigationComponents */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::CommitNavigationComponents(
        const SceneObjectId objectId, const std::optional<Runtime::NavigationSurfaceComponent> *surface,
        const std::optional<Runtime::NavigationRegionComponent> *region,
        const std::optional<Runtime::NavigationModifierComponent> *modifier, const std::optional<Runtime::NavigationLinkComponent> *link) {
        const auto object = FindObject(m_document.m_objects, objectId);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (IsEffectivelyLocked(m_document.m_objects, objectId))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        SceneObjectComponentSet candidate = object->components;
        if (surface != nullptr)
            candidate.navigationSurface = *surface;
        if (region != nullptr)
            candidate.navigationRegion = *region;
        if (modifier != nullptr)
            candidate.navigationModifier = *modifier;
        if (link != nullptr)
            candidate.navigationLink = *link;
        if (Result<void> valid = ValidateComponents(candidate); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        if (Result<void> valid =
                ValidateSceneNavigationComponents(m_document.m_objects,
                                                  std::pair{objectId, static_cast<const SceneObjectComponentSet *>(&candidate)});
            valid.HasError()) {
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        }
        if (candidate.navigationSurface == object->components.navigationSurface &&
            candidate.navigationRegion == object->components.navigationRegion &&
            candidate.navigationModifier == object->components.navigationModifier &&
            candidate.navigationLink == object->components.navigationLink) {
            return Result<SceneCommandResult>::Success(
                {objectId, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        }

        SceneCommandDelta delta = NavigationComponentsChangedDelta{
            .object = objectId,
            .surfaceBefore = object->components.navigationSurface,
            .surfaceAfter = candidate.navigationSurface,
            .regionBefore = object->components.navigationRegion,
            .regionAfter = candidate.navigationRegion,
            .modifierBefore = object->components.navigationModifier,
            .modifierAfter = candidate.navigationModifier,
            .linkBefore = object->components.navigationLink,
            .linkAfter = candidate.navigationLink,
        };
        ObserveNavigationComponentIds(candidate, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                      m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
        return CommitObject({std::move(delta), objectId, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationSurfaceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationSurfaceCommand &command) {
        return CommitNavigationComponents(command.object, &command.surface, nullptr, nullptr, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationRegionCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationRegionCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, &command.region, nullptr, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationModifierCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationModifierCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, nullptr, &command.modifier, nullptr);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneNavigationLinkCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneNavigationLinkCommand &command) {
        return CommitNavigationComponents(command.object, nullptr, nullptr, nullptr, &command.link);
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectEditorStateCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectEditorStateCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (object->editorState == command.editorState)
            return Result<SceneCommandResult>::Success(
                {command.object, m_document.m_revision, m_document.m_state, DocumentChangeKind::EditorStateChanged, {}, false});

        return CommitObject({EditorStateChangedDelta{object->id, object->editorState, command.editorState}, command.object,
                             DocumentChangeKind::EditorStateChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const AddSceneObjectComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const AddSceneObjectComponentCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        if (HasComponent(object->components, command.type)) {
            return ComponentNoOpResult(m_document, object->id);
        }

        return CommitObject({ComponentAddedDelta{object->id, command.type}, command.object, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectComponentCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectComponentCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        if (!HasComponent(object->components, command.type)) {
            return ComponentNoOpResult(m_document, object->id);
        }

        return CommitObject({ComponentRemovedDelta{object->id, command.type, object->components.camera, object->components.light,
                                                   object->components.triggerVolume, object->components.audioSource},
                             command.object, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const AttachSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const AttachSceneObjectBehaviorCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        if (!command.typeId.IsValid() || command.schemaVersion == 0 ||
            (!command.allowMultiple && std::ranges::any_of(object->components.behaviors, [&](const auto &behavior) {
            return behavior.typeId == command.typeId;
        }))) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior type cannot be attached more than once to this object."));
        }
        Gameplay::BehaviorComponent behavior{Gameplay::BehaviorInstanceId{m_document.m_nextBehaviorInstanceId}, command.typeId,
                                             command.schemaVersion, command.enabled, command.fields};
        if (Gameplay::ValidateBehaviorComponent(behavior).HasError())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment payload is invalid."));
        auto after = object->components.behaviors;
        after.push_back(std::move(behavior));
        SceneCommandDelta delta = BehaviorsChangedDelta{object->id, object->components.behaviors, std::move(after)};
        if (const Result<void> valid = ValidateHistoryDelta(delta, 1); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        ++m_document.m_nextBehaviorInstanceId;
        return CommitObject({std::move(delta), command.object, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetSceneObjectBehaviorCommand &command) {
        if (Gameplay::ValidateBehaviorComponent(command.behavior).HasError())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior replacement payload is invalid."));
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        const auto behavior =
            std::ranges::find(object->components.behaviors, command.behavior.instanceId, &Gameplay::BehaviorComponent::instanceId);
        if (behavior == object->components.behaviors.end() || behavior->typeId != command.behavior.typeId)
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment does not exist or changed type."));
        if (*behavior == command.behavior)
            return Result<SceneCommandResult>::Success(
                {command.object, m_document.m_revision, m_document.m_state, DocumentChangeKind::ComponentChanged, {}, false});
        auto after = object->components.behaviors;
        *std::ranges::find(after, command.behavior.instanceId, &Gameplay::BehaviorComponent::instanceId) = command.behavior;
        SceneCommandDelta delta = BehaviorsChangedDelta{object->id, object->components.behaviors, std::move(after)};
        if (const Result<void> valid = ValidateHistoryDelta(delta, 1); valid.HasError())
            return Result<SceneCommandResult>::Failure(valid.ErrorValue());
        return CommitObject({std::move(delta), command.object, DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectBehaviorCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const RemoveSceneObjectBehaviorCommand &command) {
        const auto object = FindObject(m_document.m_objects, command.object);
        if (object == m_document.m_objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (IsEffectivelyLocked(m_document.m_objects, command.object))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        auto after = object->components.behaviors;
        if (const auto removed = std::erase_if(after,
                                               [behaviorId = command.behavior](const Gameplay::BehaviorComponent &behavior) {
            return behavior.instanceId == behaviorId;
        });
            removed == 0)
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidBehavior, "Behavior attachment does not exist."));
        return CommitObject({BehaviorsChangedDelta{object->id, object->components.behaviors, std::move(after)}, command.object,
                             DocumentChangeKind::ComponentChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DuplicateSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DuplicateSceneObjectCommand &command) {
        if (!IsValidSceneObjectName(command.name)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Scene object name must contain 1 to 128 bytes."));
        }
        const auto source = FindObject(m_document.m_objects, command.source);
        if (source == m_document.m_objects.end()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        }
        if (IsEffectivelyLocked(m_document.m_objects, command.source))
            return Result<SceneCommandResult>::Failure(LockedObjectError());

        const SceneObjectId id{m_document.m_nextObjectId};
        SceneObjectComponentSet duplicatedComponents = source->components;
        if (Result<void> regenerated =
                RegenerateDuplicatedNavigationIdentities(duplicatedComponents, m_document.m_nextNavigationSurfaceId,
                                                         m_document.m_nextNavigationRegionId, m_document.m_nextNavigationModifierId,
                                                         m_document.m_nextNavigationLinkId);
            regenerated.HasError())
            return Result<SceneCommandResult>::Failure(regenerated.ErrorValue());
        if (Result<void> navigation = ValidateSceneNavigationComponents(m_document.m_objects, std::nullopt, &duplicatedComponents);
            navigation.HasError()) {
            return Result<SceneCommandResult>::Failure(navigation.ErrorValue());
        }
        for (Gameplay::BehaviorComponent &behavior : duplicatedComponents.behaviors)
            behavior.instanceId = Gameplay::BehaviorInstanceId{m_document.m_nextBehaviorInstanceId++};
        ObserveNavigationComponentIds(duplicatedComponents, m_document.m_nextNavigationSurfaceId, m_document.m_nextNavigationRegionId,
                                      m_document.m_nextNavigationModifierId, m_document.m_nextNavigationLinkId);
        SceneCommandDelta delta = CreatedObjectDelta{
            .object = SceneObjectSnapshot{.id = id,
                                          .parent = source->parent,
                                          .name = command.name,
                                          .localTransform = source->localTransform,
                                          .primitiveMesh = source->primitiveMesh,
                                          .components = std::move(duplicatedComponents),
                                          .meshAsset = source->meshAsset,
                                          .editorState = source->editorState},
            .index = m_document.m_objects.size(),
            .kind = DocumentChangeKind::Duplicated,
        };
        ++m_document.m_nextObjectId;
        return CommitObject({std::move(delta), id, DocumentChangeKind::Duplicated});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectCommand &command) {
        return Execute(DeleteSceneObjectsCommand{{command.object}});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectsCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteSceneObjectsCommand &command) {
        const std::vector<SceneObjectId> selected = SelectExistingObjects(m_document.m_objects, command.objects);
        if (selected.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "No requested scene object exists in the active document."));
        }

        std::vector<SceneObjectId> roots = DeletionRoots(m_document.m_objects, selected);
        std::unordered_set<std::uint64_t> removedIds;
        std::vector<SceneObjectId> removed = CollectRemovedObjects(m_document.m_objects, roots, removedIds);
        if (std::ranges::any_of(removed, [&](const SceneObjectId object) {
            return IsEffectivelyLocked(m_document.m_objects, object);
        }))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        std::vector<Runtime::NavigationSceneComponentView> remainingNavigation;
        remainingNavigation.reserve(m_document.m_objects.size() - removedIds.size());
        for (const SceneObjectSnapshot &object : m_document.m_objects) {
            if (removedIds.contains(object.id.value))
                continue;
            remainingNavigation.push_back(
                {.surface = object.components.navigationSurface ? &*object.components.navigationSurface : nullptr,
                 .region = object.components.navigationRegion ? &*object.components.navigationRegion : nullptr,
                 .modifier = object.components.navigationModifier ? &*object.components.navigationModifier : nullptr,
                 .link = object.components.navigationLink ? &*object.components.navigationLink : nullptr});
        }
        if (Result<void> navigation = Runtime::ValidateNavigationSceneComponentViews(remainingNavigation); navigation.HasError()) {
            return Result<SceneCommandResult>::Failure(navigation.ErrorValue());
        }
        const SceneObjectId primary = roots.front();
        SceneCommandDelta delta = CaptureDeletedObjects(m_document.m_objects, m_document.m_prefabInstances, std::move(roots), removedIds);
        const auto &deleted = std::get<DeletedObjectsDelta>(delta);
        std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
        affectedPrefabInstances.reserve(deleted.prefabInstances.size());
        for (const IndexedPrefabInstance &instance : deleted.prefabInstances)
            affectedPrefabInstances.push_back(instance.instance.instanceId);
        if (const Result<void> validHistory = ValidateHistoryDelta(delta, removed.size() + affectedPrefabInstances.size());
            validHistory.HasError()) {
            return Result<SceneCommandResult>::Failure(validHistory.ErrorValue());
        }
        const std::size_t memoryBytes = EstimateMemoryBytes(delta, removed.size() + affectedPrefabInstances.size());
        const DocumentStateId beforeState = m_document.m_state;
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, delta);
        ++m_document.m_revision.value;
        m_document.m_state = DocumentStateId{m_document.m_nextStateId++};
        PushHistory(*m_history.m_impl,
                    HistoryRecord{beforeState, m_document.m_state, std::move(delta), removed, memoryBytes, affectedPrefabInstances});
        SceneCommandResult result{primary, m_document.m_revision, m_document.m_state, DocumentChangeKind::Deleted, std::move(removed),
                                  true};
        result.affectedPrefabInstances = std::move(affectedPrefabInstances);
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const CreateScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const CreateScenePrefabInstanceCommand &command) {
        if (!command.sourcePrefab.IsValid()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance, "Prefab placement requires a valid asset reference."));
        }
        if (!IsValid(command.rootTransform)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Prefab root placement transform must be finite."));
        }
        if (const Result<void> validParent = ValidatePrefabParent(m_document.m_objects, command.parent); validParent.HasError())
            return Result<SceneCommandResult>::Failure(validParent.ErrorValue());
        auto instanceId = AllocatePrefabInstanceId(m_document.m_nextPrefabInstanceId);
        if (instanceId.HasError())
            return Result<SceneCommandResult>::Failure(instanceId.ErrorValue());
        SceneCommandDelta delta = CreatedPrefabInstanceDelta{
            .instance = ScenePrefabInstance{instanceId.Value(), command.sourcePrefab, command.parent, command.rootTransform},
            .index = m_document.m_prefabInstances.size(),
        };
        return CommitPrefab(PrefabCommitContext{std::move(delta), instanceId.Value(), DocumentChangeKind::PrefabInstanceCreated, true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const SetScenePrefabInstanceRootTransformCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const SetScenePrefabInstanceRootTransformCommand &command) {
        if (!IsValid(command.rootTransform)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidTransform, "Prefab root placement transform must be finite."));
        }
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        if (instance.Value()->rootTransform == command.rootTransform)
            return Result<SceneCommandResult>::Success(PrefabNoOpResult(m_document.m_revision, m_document.m_state, command.instance,
                                                                        DocumentChangeKind::PrefabInstanceTransformChanged));
        SceneCommandDelta delta = PrefabInstanceTransformDelta{command.instance, instance.Value()->rootTransform, command.rootTransform};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceTransformChanged});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DuplicateScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DuplicateScenePrefabInstanceCommand &command) {
        auto source = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.source);
        if (source.HasError())
            return Result<SceneCommandResult>::Failure(source.ErrorValue());
        auto instanceId = AllocatePrefabInstanceId(m_document.m_nextPrefabInstanceId);
        if (instanceId.HasError())
            return Result<SceneCommandResult>::Failure(instanceId.ErrorValue());
        ScenePrefabInstance duplicate = *source.Value();
        duplicate.instanceId = instanceId.Value();
        SceneCommandDelta delta = CreatedPrefabInstanceDelta{
            .instance = std::move(duplicate),
            .index = m_document.m_prefabInstances.size(),
            .kind = DocumentChangeKind::PrefabInstanceDuplicated,
        };
        return CommitPrefab(PrefabCommitContext{std::move(delta), instanceId.Value(), DocumentChangeKind::PrefabInstanceDuplicated, true});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const ReparentScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const ReparentScenePrefabInstanceCommand &command) {
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        if (const Result<void> validParent = ValidatePrefabParent(m_document.m_objects, command.parent); validParent.HasError())
            return Result<SceneCommandResult>::Failure(validParent.ErrorValue());
        if (instance.Value()->parent == command.parent)
            return Result<SceneCommandResult>::Success(PrefabNoOpResult(m_document.m_revision, m_document.m_state, command.instance,
                                                                        DocumentChangeKind::PrefabInstanceReparented));
        SceneCommandDelta delta = PrefabInstanceReparentDelta{command.instance, instance.Value()->parent, command.parent};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceReparented});
    }

    /** @copydoc SceneDocumentCommandExecutor::Execute(const DeleteScenePrefabInstanceCommand&) */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Execute(const DeleteScenePrefabInstanceCommand &command) {
        auto instance = FindEditablePrefabInstance(m_document.m_objects, m_document.m_prefabInstances, command.instance);
        if (instance.HasError())
            return Result<SceneCommandResult>::Failure(instance.ErrorValue());
        const auto index = static_cast<std::size_t>(instance.Value() - m_document.m_prefabInstances.data());
        SceneCommandDelta delta = DeletedPrefabInstancesDelta{{IndexedPrefabInstance{*instance.Value(), index}}};
        return CommitPrefab(PrefabCommitContext{std::move(delta), command.instance, DocumentChangeKind::PrefabInstanceDeleted});
    }

    /** @copydoc SceneDocumentCommandExecutor::Undo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Undo() {
        if (m_history.m_impl->undo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToUndo, "No committed scene command is available to undo."));
        }
        HistoryRecord entry = std::move(m_history.m_impl->undo.back());
        m_history.m_impl->undo.pop_back();
        RevertDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.beforeState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Undone);
        m_history.m_impl->redo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc SceneDocumentCommandExecutor::Redo */
    Result<SceneCommandResult> SceneDocumentCommandExecutor::Redo() {
        if (m_history.m_impl->redo.empty()) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::NothingToRedo, "No reverted scene command is available to redo."));
        }
        HistoryRecord entry = std::move(m_history.m_impl->redo.back());
        m_history.m_impl->redo.pop_back();
        ApplyDelta(m_document.m_objects, m_document.m_prefabInstances, entry.delta);
        ++m_document.m_revision.value;
        m_document.m_state = entry.afterState;
        SceneCommandResult result = HistoryCommandResult(m_document, entry, DocumentChangeKind::Redone);
        m_history.m_impl->undo.push_back(std::move(entry));
        return Result<SceneCommandResult>::Success(std::move(result));
    }

    /** @copydoc CreateSceneObjectUseCase::CreateSceneObjectUseCase */
    CreateSceneObjectUseCase::CreateSceneObjectUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept
        : m_document(document), m_executor(executor) {}

    /** @copydoc CreateSceneObjectUseCase::Execute */
    Result<SceneCommandResult> CreateSceneObjectUseCase::Execute(const PrimitiveCreationRequest &request) {
        const Runtime::PrimitiveDescriptor *descriptor = Runtime::PrimitiveCatalog::Find(request.primitive.value);
        if (descriptor == nullptr) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::UnknownPrimitive, "Requested primitive is not registered."));
        }
        if (descriptor->creationGroup == Runtime::PrimitiveCreationGroup::NotCreatable ||
            descriptor->category == Runtime::PrimitiveCategory::Collider) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::PrimitiveNotCreatable, "Requested primitive is not a hierarchy creation object."));
        }
        if (request.parent.has_value() && !m_document.Contains(*request.parent)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Scene object parent does not exist."));
        }

        std::optional<std::string> name = UniqueSiblingName(descriptor->defaultObjectName, request.parent, m_document.Objects());
        if (!name.has_value())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Unable to allocate a unique primitive object name."));

        CreateSceneObjectCommand command{.name = std::move(*name), .parent = request.parent};
        if (descriptor->meshType.has_value()) {
            command.primitiveMesh = PrimitiveMeshDescriptor::Defaults(*descriptor->meshType);
        } else if (descriptor->sceneObjectType.has_value()) {
            switch (*descriptor->sceneObjectType) {
                using enum Horo::Runtime::SceneObjectPrimitiveType;
                case Runtime::SceneObjectPrimitiveType::Empty:
                    break;
                case Runtime::SceneObjectPrimitiveType::Camera:
                    command.components.camera = Runtime::CameraComponent{};
                    break;
                case Runtime::SceneObjectPrimitiveType::DirectionalLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Directional};
                    break;
                case Runtime::SceneObjectPrimitiveType::PointLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Point};
                    break;
                case Runtime::SceneObjectPrimitiveType::SpotLight:
                    command.components.light = Runtime::LightComponent{.kind = Runtime::LightKind::Spot};
                    break;
                case Runtime::SceneObjectPrimitiveType::TriggerVolume:
                    command.components.triggerVolume = Runtime::TriggerVolumeComponent{};
                    break;
                case Runtime::SceneObjectPrimitiveType::AudioSource:
                    command.components.audioSource = Runtime::AudioSourceComponent{};
                    break;
            }
        } else {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata, "Creatable primitive has no typed authoring descriptor."));
        }
        return m_executor.Execute(command);
    }

    /** @copydoc InstantiateSceneAssetUseCase::InstantiateSceneAssetUseCase */
    InstantiateSceneAssetUseCase::InstantiateSceneAssetUseCase(SceneDocument &document, SceneDocumentCommandExecutor &executor) noexcept
        : document_(document), executor_(executor) {}

    /** @copydoc InstantiateSceneAssetUseCase::Execute */
    Result<SceneCommandResult> InstantiateSceneAssetUseCase::Execute(const AssetInstantiationRequest &request) {
        if (!request.asset.IsValid() || !IsValidSceneObjectName(request.baseName)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata, "Asset instantiation request is invalid."));
        }
        if (request.parent.has_value() && !document_.Contains(*request.parent)) {
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Asset drop parent no longer exists."));
        }

        std::optional<std::string> name = UniqueSiblingName(request.baseName, request.parent, document_.Objects());
        if (!name.has_value())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidName, "Unable to allocate a unique asset object name."));
        return executor_.Execute(CreateSceneObjectCommand{.name = std::move(*name),
                                                          .parent = request.parent,
                                                          .localTransform = request.localTransform,
                                                          .components = {},
                                                          .meshAsset = request.asset});
    }
}  // namespace Horo::Editor
