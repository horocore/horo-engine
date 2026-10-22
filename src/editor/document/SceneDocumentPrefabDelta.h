#pragma once

#include "editor/document/SceneDocumentInternalCore.h"

// Placement mutations share the canonical delta owner; this header owns no document/history state.
namespace Horo::Editor::SceneDocumentDetail {
    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
        const std::size_t index = std::min(delta.index, instances.size());
        instances.insert(instances.begin() + static_cast<std::ptrdiff_t>(index), delta.instance);
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->rootTransform = delta.after;
    }

    inline void ApplyPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceValuesDelta &delta) {
        auto prepared = delta.after;
        FindPrefabInstance(instances, delta.instance)->valueOverrides.swap(prepared);
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

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const CreatedPrefabInstanceDelta &delta) {
        std::erase_if(instances, [&delta](const ScenePrefabInstance &instance) {
            return instance.instanceId == delta.instance.instanceId;
        });
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceTransformDelta &delta) {
        FindPrefabInstance(instances, delta.instance)->rootTransform = delta.before;
    }

    inline void RevertPrefabTypedDelta(std::vector<ScenePrefabInstance> &instances, const PrefabInstanceValuesDelta &delta) {
        auto prepared = delta.before;
        FindPrefabInstance(instances, delta.instance)->valueOverrides.swap(prepared);
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

}  // namespace Horo::Editor::SceneDocumentDetail
