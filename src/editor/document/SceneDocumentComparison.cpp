#include "editor/document/SceneDocumentComparison.h"

#include "editor/document/SceneDocumentPersistence.h"

#include <unordered_map>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneComparisonDomain{"horo.editor.scene_comparison"};
        const ErrorCodeDescriptor SceneComparisonUnavailable{
            .domain = SceneComparisonDomain,
            .code = ErrorCode{"scene_comparison.unavailable"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The canonical scene is unavailable for comparison.",
        };

        [[nodiscard]] SceneObjectDifferenceFields CompareObjectFields(const SceneObjectSnapshot &document,
                                                                      const SceneObjectSnapshot &disk) {
            return {
                .name = document.name != disk.name,
                .parent = document.parent != disk.parent,
                .transform = document.localTransform != disk.localTransform,
                .primitive = document.primitiveMesh != disk.primitiveMesh,
                .asset = document.meshAsset != disk.meshAsset,
                .components = document.components != disk.components,
                .editorState = document.editorState != disk.editorState,
                .generatedOwnership = document.generated != disk.generated,
            };
        }

        [[nodiscard]] ScenePrefabInstanceDifferenceFields ComparePrefabInstanceFields(const ScenePrefabInstance &document,
                                                                                      const ScenePrefabInstance &disk) {
            return {
                .sourcePrefab = document.sourcePrefab != disk.sourcePrefab,
                .parent = document.parent != disk.parent,
                .rootTransform = document.rootTransform != disk.rootTransform,
            };
        }
    }  // namespace

    /** @copydoc CompareSceneDocuments */
    SceneDocumentComparison CompareSceneDocuments(const SceneDocumentSnapshot &document, const SceneDocumentSnapshot &disk) {
        std::unordered_map<std::uint64_t, const SceneObjectSnapshot *> diskObjects;
        diskObjects.reserve(disk.objects.size());
        for (const SceneObjectSnapshot &object : disk.objects)
            diskObjects.emplace(object.id.value, &object);

        SceneDocumentComparison comparison;
        comparison.objects.reserve(document.objects.size() + disk.objects.size());
        for (const SceneObjectSnapshot &documentObject : document.objects) {
            const auto diskObject = diskObjects.find(documentObject.id.value);
            if (diskObject == diskObjects.end()) {
                comparison.objects.push_back({
                    .id = documentObject.id,
                    .kind = SceneObjectComparisonKind::RemovedFromDisk,
                    .documentName = documentObject.name,
                });
                ++comparison.removedFromDisk;
                continue;
            }

            if (const SceneObjectDifferenceFields fields = CompareObjectFields(documentObject, *diskObject->second); fields.Any()) {
                comparison.objects.push_back({
                    .id = documentObject.id,
                    .kind = SceneObjectComparisonKind::Modified,
                    .documentName = documentObject.name,
                    .diskName = diskObject->second->name,
                    .fields = fields,
                });
                ++comparison.modified;
            }

            diskObjects.erase(diskObject);
        }

        for (const SceneObjectSnapshot &diskObject : disk.objects) {
            if (!diskObjects.contains(diskObject.id.value))
                continue;
            comparison.objects.push_back({
                .id = diskObject.id,
                .kind = SceneObjectComparisonKind::AddedOnDisk,
                .diskName = diskObject.name,
            });
            ++comparison.addedOnDisk;
        }

        std::unordered_map<std::uint64_t, const ScenePrefabInstance *> diskPrefabInstances;
        diskPrefabInstances.reserve(disk.prefabInstances.size());
        for (const ScenePrefabInstance &instance : disk.prefabInstances)
            diskPrefabInstances.emplace(instance.instanceId.Value(), &instance);

        comparison.prefabInstances.reserve(document.prefabInstances.size() + disk.prefabInstances.size());
        for (const ScenePrefabInstance &documentInstance : document.prefabInstances) {
            const auto diskInstance = diskPrefabInstances.find(documentInstance.instanceId.Value());
            if (diskInstance == diskPrefabInstances.end()) {
                comparison.prefabInstances.push_back({
                    .id = documentInstance.instanceId,
                    .kind = SceneObjectComparisonKind::RemovedFromDisk,
                });
                ++comparison.prefabInstancesRemovedFromDisk;
                continue;
            }

            if (const ScenePrefabInstanceDifferenceFields fields = ComparePrefabInstanceFields(documentInstance, *diskInstance->second);
                fields.Any()) {
                comparison.prefabInstances.push_back({
                    .id = documentInstance.instanceId,
                    .kind = SceneObjectComparisonKind::Modified,
                    .fields = fields,
                });
                ++comparison.prefabInstancesModified;
            }

            diskPrefabInstances.erase(diskInstance);
        }

        for (const ScenePrefabInstance &diskInstance : disk.prefabInstances) {
            if (!diskPrefabInstances.contains(diskInstance.instanceId.Value()))
                continue;
            comparison.prefabInstances.push_back({
                .id = diskInstance.instanceId,
                .kind = SceneObjectComparisonKind::AddedOnDisk,
            });
            ++comparison.prefabInstancesAddedOnDisk;
        }
        return comparison;
    }

    /** @copydoc LoadSceneDocumentComparison */
    Result<SceneDocumentComparison> LoadSceneDocumentComparison(const SceneDocumentComparisonRequest &request) {
        if (!request.absoluteProjectRoot.is_absolute() || !request.absoluteScenePath.is_absolute()) {
            return Result<SceneDocumentComparison>::Failure(MakeError(SceneComparisonUnavailable));
        }

        Result<std::optional<LoadedProjectScene>> loaded = LoadProjectDefaultScene(request.absoluteProjectRoot);
        if (loaded.HasError())
            return Result<SceneDocumentComparison>::Failure(loaded.ErrorValue());
        if (!loaded.Value().has_value() ||
            loaded.Value()->absolutePath.lexically_normal() != request.absoluteScenePath.lexically_normal()) {
            return Result<SceneDocumentComparison>::Failure(MakeError(SceneComparisonUnavailable));
        }

        LoadedProjectScene diskScene = std::move(loaded).Value().value();
        SceneDocumentSnapshot disk{
            .objects = std::move(diskScene.objects),
            .prefabInstances = std::move(diskScene.prefabInstances),
        };
        SceneDocumentComparison comparison = CompareSceneDocuments(request.document, disk);
        comparison.absoluteScenePath = request.absoluteScenePath.string();
        return Result<SceneDocumentComparison>::Success(std::move(comparison));
    }
}  // namespace Horo::Editor
