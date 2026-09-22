#pragma once

/**
 * @file SceneDocumentInternal.h
 * @brief Private shared implementation types for the scene document command units.
 */

#include "Horo/AI/AIErrors.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/document/SceneDocument.h"
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

namespace Horo::Editor::SceneDocumentDetail {
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
        std::optional<Runtime::NavigationAgentComponent> agentBefore;
        std::optional<Runtime::NavigationAgentComponent> agentAfter;
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

    struct GameplayComponentsChangedDelta {
        SceneObjectId object;
        std::vector<Gameplay::SerializedComponent> before;
        std::vector<Gameplay::SerializedComponent> after;
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
                     EditorStateChangedDelta, ComponentAddedDelta, ComponentRemovedDelta, BehaviorsChangedDelta,
                     GameplayComponentsChangedDelta, DeletedObjectsDelta, CreatedPrefabInstanceDelta, PrefabInstanceTransformDelta,
                     PrefabInstanceReparentDelta, DeletedPrefabInstancesDelta>;

    struct HistoryRecord {
        DocumentStateId beforeState;
        DocumentStateId afterState;
        SceneCommandDelta delta;
        std::vector<SceneObjectId> affectedObjects;
        std::size_t memoryBytes{0};
        std::vector<Prefab::PrefabInstanceId> affectedPrefabInstances;
    };

    [[nodiscard]] inline Error MakeDocumentError(const ErrorCodeDescriptor &descriptor, std::string message) {
        return MakeError(descriptor, std::move(message));
    }

    [[nodiscard]] inline bool IsValid(const Math::Transform &transform) noexcept {
        return Math::IsFinite(transform.translation) && Math::IsFinite(transform.scale) && transform.rotation.TryNormalized().HasValue();
    }

    [[nodiscard]] inline auto FindObject(std::vector<SceneObjectSnapshot> &objects, const SceneObjectId id) {
        return std::ranges::find(objects, id, &SceneObjectSnapshot::id);
    }

    [[nodiscard]] inline auto FindObject(const std::vector<SceneObjectSnapshot> &objects, const SceneObjectId id) {
        return std::ranges::find(objects, id, &SceneObjectSnapshot::id);
    }

    [[nodiscard]] inline auto FindPrefabInstance(std::vector<ScenePrefabInstance> &instances, const Prefab::PrefabInstanceId id) {
        return std::ranges::find(instances, id, &ScenePrefabInstance::instanceId);
    }

    [[nodiscard]] inline auto FindPrefabInstance(const std::vector<ScenePrefabInstance> &instances, const Prefab::PrefabInstanceId id) {
        return std::ranges::find(instances, id, &ScenePrefabInstance::instanceId);
    }

    [[nodiscard]] inline bool IsEffectivelyLocked(const std::vector<SceneObjectSnapshot> &objects, const SceneObjectId object) noexcept {
        const std::optional<ResolvedSceneObjectEditorState> state = ResolveSceneObjectEditorState(objects, object);
        return state.has_value() && state->effectivelyLocked;
    }

    [[nodiscard]] inline bool IsPrefabInstanceLocked(const std::vector<SceneObjectSnapshot> &objects,
                                                     const ScenePrefabInstance &instance) noexcept {
        return instance.parent.has_value() && IsEffectivelyLocked(objects, *instance.parent);
    }

    [[nodiscard]] inline Error LockedObjectError() {
        return MakeDocumentError(SceneDocumentErrors::ObjectLocked, "Scene object or one of its ancestors is locked in the editor.");
    }

    [[nodiscard]] inline Result<SceneCommandResult> ComponentNoOpResult(const SceneDocument &document, const SceneObjectId object) {
        return Result<SceneCommandResult>::Success(
            {object, document.Revision(), document.State(), DocumentChangeKind::ComponentChanged, {}, false});
    }

    [[nodiscard]] inline Result<void> ValidateDescriptor(const std::optional<PrimitiveMeshDescriptor> &descriptor) {
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

    [[nodiscard]] inline Result<void> ValidatePhysicsAuthoringComponents(const SceneObjectComponentSet &components) {
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

    [[nodiscard]] inline Result<void> ValidateComponents(const SceneObjectComponentSet &components) {
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
        if (components.navigationAgent && Runtime::ValidateNavigationAgentComponent(*components.navigationAgent).HasError())
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::AgentDescriptorInvalid));
        if (components.aiAgent && AI::ValidateAiAgentComponent(*components.aiAgent).HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::SceneComponentInvalid));
        if (components.aiController && AI::ValidateAiControllerComponent(*components.aiController).HasError())
            return Result<void>::Failure(MakeError(AI::AIErrors::SceneComponentInvalid));
        if (components.camera.has_value()) {
            const Runtime::CameraComponent &camera = *components.camera;
            if (!IsValidCameraComponent(camera)) {
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidCamera, "Camera authoring values are invalid."));
            }
        }
        if (components.light.has_value()) {
            const Runtime::LightComponent &light = *components.light;
            if (!IsValidLightComponent(light)) {
                return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidLight, "Light authoring values are invalid."));
            }
        }
        if (components.audioSource.has_value() &&
            (Audio::ValidateAudioSoundReference(components.audioSource->sound).HasError() ||
             Audio::ValidateAudioSoundPlaybackDefaults(components.audioSource->playback).HasError() ||
             Audio::ValidateAudioSceneLifecyclePolicy(components.audioSource->sceneLifecycle).HasError())) {
            return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidAudioSource, "Audio source values are invalid."));
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
        if (const Result<void> gameplay = Gameplay::ValidateSerializedComponents(components.gameplayComponents); gameplay.HasError())
            return gameplay;
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> ValidateSceneNavigationComponents(
        const std::span<const SceneObjectSnapshot> objects,
        const std::optional<std::pair<SceneObjectId, const SceneObjectComponentSet *>> replacement = std::nullopt,
        const SceneObjectComponentSet *appended = nullptr) {
        std::vector<Runtime::NavigationSceneComponentView> views;
        views.reserve(objects.size() + (appended != nullptr ? 1U : 0U));
        const auto collect = [&](const SceneObjectComponentSet &components) {
            views.push_back({.surface = components.navigationSurface ? &*components.navigationSurface : nullptr,
                             .region = components.navigationRegion ? &*components.navigationRegion : nullptr,
                             .modifier = components.navigationModifier ? &*components.navigationModifier : nullptr,
                             .link = components.navigationLink ? &*components.navigationLink : nullptr,
                             .agent = components.navigationAgent ? &*components.navigationAgent : nullptr});
        };
        for (const SceneObjectSnapshot &object : objects) {
            collect(replacement && replacement->first == object.id ? *replacement->second : object.components);
        }
        if (appended != nullptr)
            collect(*appended);
        return Runtime::ValidateNavigationSceneComponentViews(views);
    }

    [[nodiscard]] inline Result<void> ValidateSceneAiComponents(
        const std::span<const SceneObjectSnapshot> objects,
        const std::optional<std::pair<SceneObjectId, const SceneObjectComponentSet *>> replacement = std::nullopt,
        const SceneObjectComponentSet *appended = nullptr) {
        std::vector<AI::AiSceneComponentView> views;
        views.reserve(objects.size() + (appended != nullptr ? 1U : 0U));
        const auto collect = [&](const SceneObjectComponentSet &components) {
            views.push_back({.agent = components.aiAgent ? &*components.aiAgent : nullptr,
                             .controller = components.aiController ? &*components.aiController : nullptr});
        };
        for (const SceneObjectSnapshot &object : objects)
            collect(replacement && replacement->first == object.id ? *replacement->second : object.components);
        if (appended != nullptr)
            collect(*appended);
        return AI::ValidateAiSceneComponents(views);
    }

    inline void ObserveAiAgentId(const SceneObjectComponentSet &components, std::uint64_t &nextAgentId) noexcept {
        if (!components.aiAgent)
            return;
        if (nextAgentId != 0 && components.aiAgent->agent.Value() >= nextAgentId)
            nextAgentId =
                components.aiAgent->agent.Value() == std::numeric_limits<std::uint64_t>::max() ? 0 : components.aiAgent->agent.Value() + 1;
    }

    inline void ObserveNavigationComponentIds(const SceneObjectComponentSet &components, std::uint64_t &nextSurfaceId,
                                              std::uint64_t &nextRegionId, std::uint64_t &nextModifierId,
                                              std::uint64_t &nextLinkId) noexcept {
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
    inline void RetargetLocalNavigationSurface(SceneObjectComponentSet &components, const Navigation::SurfaceId sourceSurface,
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
    [[nodiscard]] inline Result<void> RegenerateDuplicatedNavigationIdentities(SceneObjectComponentSet &components,
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

}  // namespace Horo::Editor::SceneDocumentDetail
