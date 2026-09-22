#include "editor/document/SceneDocument.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/document/SceneDocumentInternal.h"
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
    using namespace SceneDocumentDetail;

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
               Audio::ValidateAudioSoundPlaybackDefaults(audioSource.playback).HasValue() &&
               Audio::ValidateAudioSceneLifecyclePolicy(audioSource.sceneLifecycle).HasValue();
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

    /** @copydoc InspectSceneGameplayComponents */
    SceneGameplayInspection InspectSceneGameplayComponents(const std::span<const SceneObjectSnapshot> objects,
                                                           const Gameplay::ComponentRegistry &registry) {
        SceneGameplayInspection inspection;
        for (const SceneObjectSnapshot &object : objects) {
            for (std::size_t componentIndex = 0; componentIndex < object.components.gameplayComponents.size(); ++componentIndex) {
                const Gameplay::SerializedComponent &component = object.components.gameplayComponents[componentIndex];
                const Result<Gameplay::ComponentInspection> result = registry.Inspect(component);
                if (result.HasError()) {
                    inspection.issues.push_back(SceneGameplayComponentIssue{.object = object.id,
                                                                            .componentIndex = componentIndex,
                                                                            .typeId = component.typeId,
                                                                            .schemaVersion = component.schemaVersion,
                                                                            .status = Gameplay::ComponentInspectionStatus::InvalidEnvelope,
                                                                            .validationError = result.ErrorValue()});
                    continue;
                }
                if (result.Value().status != Gameplay::ComponentInspectionStatus::Current) {
                    inspection.issues.push_back(SceneGameplayComponentIssue{.object = object.id,
                                                                            .componentIndex = componentIndex,
                                                                            .typeId = component.typeId,
                                                                            .schemaVersion = component.schemaVersion,
                                                                            .status = result.Value().status});
                }
            }
        }
        std::ranges::sort(inspection.issues, [](const SceneGameplayComponentIssue &left, const SceneGameplayComponentIssue &right) {
            if (left.object != right.object)
                return left.object < right.object;
            if (left.typeId != right.typeId)
                return left.typeId < right.typeId;
            return left.componentIndex < right.componentIndex;
        });
        return inspection;
    }

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
        std::uint64_t maximumAiAgentId = 0;
        for (const SceneObjectSnapshot &object : objects) {
            if (Result<void> valid = ValidateLoadedObject(object, objectIds, behaviorIds, maximumObjectId, maximumBehaviorId);
                valid.HasError())
                return valid;
            ObserveNavigationComponentIds(object.components, nextNavigationSurfaceId, nextNavigationRegionId, nextNavigationModifierId,
                                          nextNavigationLinkId);
            if (object.components.aiAgent)
                maximumAiAgentId = std::max(maximumAiAgentId, object.components.aiAgent->agent.Value());
        }
        if (maximumObjectId == std::numeric_limits<std::uint64_t>::max() ||
            maximumBehaviorId == std::numeric_limits<std::uint64_t>::max() ||
            maximumAiAgentId == std::numeric_limits<std::uint64_t>::max()) {
            return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::ObjectNotFound,
                                                           "Loaded scene object IDs must leave space for future authored objects."));
        }

        if (Result<void> validHierarchy = ValidateLoadedHierarchy(objects, objectIds); validHierarchy.HasError())
            return validHierarchy;
        if (Result<void> navigation = ValidateSceneNavigationComponents(objects); navigation.HasError())
            return navigation;
        if (Result<void> ai = ValidateSceneAiComponents(objects); ai.HasError())
            return ai;
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
        m_nextAiAgentId = maximumAiAgentId == std::numeric_limits<std::uint64_t>::max() ? 0 : maximumAiAgentId + 1;
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

}  // namespace Horo::Editor
