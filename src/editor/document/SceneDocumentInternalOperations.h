#pragma once

#include "editor/document/SceneDocumentInternalDelta.h"

namespace Horo::Editor::SceneDocumentDetail {
    [[nodiscard]] inline Result<void> ValidateLoadedBehaviors(const SceneObjectSnapshot &object,
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

    [[nodiscard]] inline Result<void> ValidateLoadedObject(const SceneObjectSnapshot &object, std::unordered_set<std::uint64_t> &objectIds,
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
            return Result<void>::Failure(MakeDocumentError(SceneDocumentErrors::InvalidPrimitiveMetadata,
                                                           "Loaded scene object mesh references must be valid and mutually exclusive."));
        }
        if (const Result<void> components = ValidateComponents(object.components); components.HasError())
            return components;
        if (Result<void> behaviors = ValidateLoadedBehaviors(object, behaviorIds, maximumBehaviorId); behaviors.HasError())
            return behaviors;
        maximumObjectId = std::max(maximumObjectId, object.id.value);
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> ValidateLoadedObjectHierarchy(const std::vector<SceneObjectSnapshot> &objects,
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

    [[nodiscard]] inline Result<void> ValidateLoadedHierarchy(const std::vector<SceneObjectSnapshot> &objects,
                                                              const std::unordered_set<std::uint64_t> &objectIds) {
        for (const SceneObjectSnapshot &object : objects) {
            if (Result<void> valid = ValidateLoadedObjectHierarchy(objects, objectIds, object); valid.HasError())
                return valid;
        }
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<std::uint64_t> ValidateLoadedPrefabInstances(const std::vector<ScenePrefabInstance> &instances,
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

    [[nodiscard]] inline std::vector<SceneObjectId> SelectExistingObjects(const std::vector<SceneObjectSnapshot> &objects,
                                                                          const std::span<const SceneObjectId> requested) {
        std::vector<SceneObjectId> selected;
        selected.reserve(requested.size());
        for (const SceneObjectId object : requested) {
            if (!object.IsValid() || FindObject(objects, object) == objects.end() || std::ranges::find(selected, object) != selected.end())
                continue;
            selected.push_back(object);
        }
        return selected;
    }

    [[nodiscard]] inline std::unordered_set<std::uint64_t> ObjectIdSet(const std::span<const SceneObjectId> objects) {
        std::unordered_set<std::uint64_t> ids;
        ids.reserve(objects.size());
        for (const SceneObjectId object : objects)
            ids.insert(object.value);
        return ids;
    }

    [[nodiscard]] inline bool HasSelectedAncestor(const std::vector<SceneObjectSnapshot> &objects,
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

    [[nodiscard]] inline std::vector<SceneObjectId> DeletionRoots(const std::vector<SceneObjectSnapshot> &objects,
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

    [[nodiscard]] inline std::vector<SceneObjectId> CollectRemovedObjects(const std::vector<SceneObjectSnapshot> &objects,
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

    template <typename Callback>
    [[nodiscard]] inline Result<SceneCommandResult> WithEditableObject([[maybe_unused]] SceneDocument &document,
                                                                       std::vector<SceneObjectSnapshot> &objects,
                                                                       const SceneObjectId objectId, Callback &&callback) {
        const auto object = FindObject(objects, objectId);
        if (object == objects.end())
            return Result<SceneCommandResult>::Failure(
                MakeDocumentError(SceneDocumentErrors::ObjectNotFound, "Scene object does not exist."));
        if (IsEffectivelyLocked(objects, objectId))
            return Result<SceneCommandResult>::Failure(LockedObjectError());
        return std::forward<Callback>(callback)(*object);
    }

    [[nodiscard]] inline Result<void> ValidateRemainingNavigation(const std::vector<SceneObjectSnapshot> &objects,
                                                                  const std::unordered_set<std::uint64_t> &removedIds) {
        std::vector<Runtime::NavigationSceneComponentView> remaining;
        remaining.reserve(objects.size() - removedIds.size());
        for (const SceneObjectSnapshot &object : objects) {
            if (removedIds.contains(object.id.value))
                continue;
            remaining.push_back({.surface = object.components.navigationSurface ? &*object.components.navigationSurface : nullptr,
                                 .region = object.components.navigationRegion ? &*object.components.navigationRegion : nullptr,
                                 .modifier = object.components.navigationModifier ? &*object.components.navigationModifier : nullptr,
                                 .link = object.components.navigationLink ? &*object.components.navigationLink : nullptr});
        }
        return Runtime::ValidateNavigationSceneComponentViews(remaining);
    }

    [[nodiscard]] inline DeletedObjectsDelta CaptureDeletedObjects(const std::vector<SceneObjectSnapshot> &objects,
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

    [[nodiscard]] inline std::optional<std::string> UniqueSiblingName(const std::string_view baseName,
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

    template <typename History> inline void ClearRedo(History &history) noexcept {
        for (const HistoryRecord &entry : history.redo) {
            history.memoryBytes -= entry.memoryBytes;
        }
        history.redo.clear();
    }

    template <typename History> inline void PushHistory(History &history, HistoryRecord entry) {
        ClearRedo(history);
        history.memoryBytes += entry.memoryBytes;
        history.undo.push_back(std::move(entry));
        while (history.undo.size() > kMaximumHistoryEntries || history.memoryBytes > kMaximumHistoryBytes) {
            history.memoryBytes -= history.undo.front().memoryBytes;
            history.undo.erase(history.undo.begin());
        }
    }

    [[nodiscard]] inline Result<void> ValidateHistoryDelta(const SceneCommandDelta &delta, const std::size_t affectedObjectCount) {
        if (EstimateMemoryBytes(delta, affectedObjectCount) > kMaximumHistoryBytes) {
            return Result<void>::Failure(
                MakeDocumentError(SceneDocumentErrors::HistoryEntryTooLarge, "Scene command exceeds the semantic history memory budget."));
        }
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> ValidatePrefabParent(const std::vector<SceneObjectSnapshot> &objects,
                                                           const std::optional<SceneObjectId> parent) {
        if (parent.has_value() && FindObject(objects, *parent) == objects.end()) {
            return Result<void>::Failure(
                MakeDocumentError(SceneDocumentErrors::ParentNotFound, "Prefab root parent must be a containing-scene object."));
        }
        if (parent.has_value() && IsEffectivelyLocked(objects, *parent))
            return Result<void>::Failure(LockedObjectError());
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<ScenePrefabInstance *> FindEditablePrefabInstance(const std::vector<SceneObjectSnapshot> &objects,
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

    [[nodiscard]] inline Result<Prefab::PrefabInstanceId> AllocatePrefabInstanceId(const std::uint64_t nextInstanceId) {
        if (nextInstanceId == std::numeric_limits<std::uint64_t>::max()) {
            return Result<Prefab::PrefabInstanceId>::Failure(
                MakeDocumentError(SceneDocumentErrors::InvalidPrefabInstance, "Prefab instance identity space is exhausted."));
        }
        return Prefab::PrefabInstanceId::Create(nextInstanceId);
    }

    [[nodiscard]] inline SceneCommandResult PrefabNoOpResult(const DocumentRevision revision, const DocumentStateId state,
                                                             const Prefab::PrefabInstanceId instance, const DocumentChangeKind kind) {
        SceneCommandResult result{{}, revision, state, kind, {}, false};
        result.prefabInstance = instance;
        return result;
    }

    [[nodiscard]] inline SceneCommandResult HistoryCommandResult(const SceneDocument &document, const HistoryRecord &entry,
                                                                 const DocumentChangeKind kind) {
        SceneCommandResult result{DeltaRootObject(entry.delta), document.Revision(), document.State(), kind, entry.affectedObjects, true};
        result.prefabInstance = DeltaRootPrefabInstance(entry.delta);
        result.affectedPrefabInstances = entry.affectedPrefabInstances;
        return result;
    }

}  // namespace Horo::Editor::SceneDocumentDetail
