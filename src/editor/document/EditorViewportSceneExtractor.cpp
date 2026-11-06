#include "editor/document/EditorViewportSceneExtractor.h"

#include "EditorRenderExtractionErrors.h"
#include "Horo/Runtime/Render/LightSceneExtraction.h"
#include "editor/renderer/xr/EditorXrTrackingRoot.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error ExtractionError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] Result<Math::Mat4> ResolveWorld(const Runtime::RuntimeSceneView &scene, const Runtime::EntityRef entity,
                                                      const std::span<const SceneObjectTransformPreview> previews,
                                                      const std::size_t remainingDepth) {
            if (remainingDepth == 0)
                return Result<Math::Mat4>::Failure(
                    ExtractionError(ViewportSceneErrors::HierarchyCycle, "Runtime scene hierarchy contains a cycle."));
            const Result<Runtime::RuntimeEntityView> value = scene.Get(entity);
            if (value.HasError())
                return Result<Math::Mat4>::Failure(value.ErrorValue());

            const Runtime::RuntimeEntityView &view = value.Value();
            const Math::Transform *local = view.localTransform;
            if (view.authoredObject) {
                const auto preview =
                    std::ranges::find(previews, view.authoredObject->value, [](const SceneObjectTransformPreview &candidate) {
                    return candidate.object.value;
                });
                if (preview != previews.end())
                    local = &preview->localTransform;
            }
            const Math::Mat4 localToParent = local->ToMatrix();
            if (!view.parent)
                return Result<Math::Mat4>::Success(localToParent);
            Result<Math::Mat4> parent = ResolveWorld(scene, *view.parent, previews, remainingDepth - 1);
            if (parent.HasError())
                return parent;
            return Result<Math::Mat4>::Success(Math::Multiply(parent.Value(), localToParent));
        }

        [[nodiscard]] Result<void> ExtractEntityLight(const Runtime::RuntimeEntityView &entity,
                                                      const std::span<const Render::IdentifiedRenderLight> lights,
                                                      EditorViewportSceneSnapshot &extracted) {
            if (!entity.components->light)
                return Result<void>::Success();
            if (!entity.authoredObject)
                return Result<void>::Failure(
                    ExtractionError(ViewportSceneErrors::InvalidObjectId, "Viewport light has no authored object identity."));
            extracted.eligibleLights.push_back({entity.entity, SceneObjectId{entity.authoredObject->value}});
            if (!entity.components->light->enabled)
                return Result<void>::Success();
            const Render::RenderLightIdentity identity{(std::uint64_t{entity.entity.entity.generation} << 32U) |
                                                       entity.entity.entity.index};
            const auto found = std::ranges::lower_bound(lights, identity, {}, &Render::IdentifiedRenderLight::identity);
            if (found == lights.end() || found->identity != identity)
                return Result<void>::Failure(
                    ExtractionError(ViewportSceneErrors::InvalidResult, "Viewport light is missing from the exact canonical scene table."));
            auto light = found->light;
            light.stableIdentity = identity.value;
            extracted.lights.push_back(light);
            extracted.lightObjects.push_back(SceneObjectId{entity.authoredObject->value});
            return Result<void>::Success();
        }

        /** @brief Sorts light values and their picking identities together at snapshot creation, with finite stack scratch. */
        void SortExtractedLights(EditorViewportSceneSnapshot &snapshot) {
            std::array<std::size_t, Render::MaximumSceneLights> order{};
            std::array<Render::RenderLight, Render::MaximumSceneLights> lights;
            std::array<SceneObjectId, Render::MaximumSceneLights> objects;
            for (std::size_t index = 0; index < snapshot.lights.size(); ++index)
                order[index] = index;
            std::ranges::sort(std::span{order}.first(snapshot.lights.size()), {}, [&snapshot](const std::size_t index) {
                return snapshot.lights[index].stableIdentity;
            });
            for (std::size_t index = 0; index < snapshot.lights.size(); ++index) {
                lights[index] = snapshot.lights[order[index]];
                objects[index] = snapshot.lightObjects[order[index]];
            }
            std::ranges::copy(std::span{lights}.first(snapshot.lights.size()), snapshot.lights.begin());
            std::ranges::copy(std::span{objects}.first(snapshot.lightObjects.size()), snapshot.lightObjects.begin());
        }

        [[nodiscard]] Result<void> ExtractPrimitiveMeshInstance(const Runtime::RuntimeSceneView &scene,
                                                                const Runtime::RuntimeEntityView &entity,
                                                                const std::optional<ResolvedSceneObjectEditorState> &editorState,
                                                                Runtime::PrimitiveMeshCache &meshCache,
                                                                EditorViewportSceneSnapshot &extracted,
                                                                const EditorGeometryExtractionPolicy policy) {
            if (!*entity.primitiveMesh)
                return Result<void>::Success();
            if (!entity.authoredObject) {
                return Result<void>::Failure(ExtractionError(ViewportSceneErrors::InvalidObjectId,
                                                             "Renderable editor runtime entity has no authored object identity."));
            }

            if (extracted.instances.size() >= policy.maximumInstances)
                return Result<void>::Failure(ExtractionError(ViewportSceneErrors::InvalidResult, "Geometry instance capacity exceeded."));
            Result<Runtime::PrimitiveMeshLease> acquired = meshCache.Acquire(**entity.primitiveMesh);
            if (acquired.HasError()) {
                Error error = acquired.ErrorValue();
                error.message = std::format("Scene object {}: {}", entity.authoredObject->value, error.message);
                return Result<void>::Failure(std::move(error));
            }
            Runtime::PrimitiveMeshLease lease = std::move(acquired).Value();
            const Render::RenderMeshSourceHandle handle{lease.Id(), 1};
            if (std::ranges::find(extracted.meshResources, handle, &EditorViewportMeshResourceView::handle) ==
                extracted.meshResources.end()) {
                const Render::MeshData &mesh = lease.Data();
                extracted.meshResources.emplace_back(handle, mesh.vertices, mesh.indices, mesh.localBounds);
                extracted.meshLeases.push_back(std::move(lease));
            }
            const auto resource = std::ranges::find(extracted.meshResources, handle, &EditorViewportMeshResourceView::handle);
            const Result<Math::Mat4> world = ResolveWorld(scene, entity.entity, {}, scene.SlotCount() + 1);
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            extracted.instances.push_back(
                EditorViewportInstance{handle, world.Value(), resource->localBounds, Render::CoreDefaultMaterial, {}});
            extracted.instanceObjects.push_back(SceneObjectId{entity.authoredObject->value});
            extracted.instanceEntities.push_back(entity.entity);
            extracted.instancePickable.push_back(editorState.has_value() && editorState->effectivelyLocked ? 0U : 1U);
            return Result<void>::Success();
        }

        /** @brief Retains one deduplicated imported resource in the same extraction snapshot before using its borrowed view. */
        Result<void> RetainImportedMesh(const EditorAssetMeshCache &assets, const Assets::AssetId &asset, const EditorAssetMeshView &mesh,
                                        EditorViewportSceneSnapshot &extracted) {
            if (std::ranges::find(extracted.meshResources, mesh.handle, &EditorViewportMeshResourceView::handle) !=
                extracted.meshResources.end())
                return Result<void>::Success();
            auto lease = assets.Acquire(asset);
            if (lease.HasError())
                return Result<void>::Failure(lease.ErrorValue());
            extracted.importedMeshLeases.push_back(std::move(lease).Value());
            extracted.meshResources.emplace_back(mesh.handle, mesh.mesh->vertices, mesh.mesh->indices, mesh.mesh->localBounds);
            return Result<void>::Success();
        }

        Result<void> AppendDocumentAssetMesh(const Runtime::RuntimeSceneView &scene, const SceneObjectSnapshot &object,
                                             const EditorAssetMeshCache &assetMeshes,
                                             const std::optional<ResolvedSceneObjectEditorState> &editorState,
                                             EditorViewportSceneSnapshot &extracted, const EditorGeometryExtractionPolicy policy) {
            const auto assetMesh = assetMeshes.Find(*object.meshAsset);
            if (!assetMesh.has_value() || assetMesh->mesh == nullptr) {
                if (policy.requireImportedMeshes)
                    return Result<void>::Failure(
                        ExtractionError(ViewportSceneErrors::InvalidResult, "Required imported mesh is not loaded."));
                return Result<void>::Success();
            }
            if (extracted.instances.size() >= policy.maximumInstances)
                return Result<void>::Failure(ExtractionError(ViewportSceneErrors::InvalidResult, "Geometry instance capacity exceeded."));
            const auto entity = scene.Find(Runtime::SceneObjectId{object.id.value});
            if (!entity)
                return Result<void>::Failure(
                    ExtractionError(ViewportSceneErrors::ObjectNotFound, "Imported mesh scene object does not exist."));
            if (auto retained = RetainImportedMesh(assetMeshes, *object.meshAsset, *assetMesh, extracted); retained.HasError())
                return retained;
            const auto world = ResolveWorld(scene, *entity, {}, scene.SlotCount() + 1);
            if (world.HasError())
                return Result<void>::Failure(world.ErrorValue());
            extracted.instances.push_back(
                EditorViewportInstance{assetMesh->handle, world.Value(), assetMesh->mesh->localBounds, Render::CoreDefaultMaterial, {}});
            extracted.instanceObjects.push_back(object.id);
            extracted.instanceEntities.push_back(*entity);
            extracted.instancePickable.push_back(editorState.has_value() && editorState->effectivelyLocked ? 0U : 1U);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ExtractDocumentAssetMeshes(const Runtime::RuntimeSceneView &scene, const SceneDocumentSnapshot &document,
                                                              const EditorAssetMeshCache &assetMeshes, const bool respectEditorVisibility,
                                                              EditorViewportSceneSnapshot &extracted,
                                                              const EditorGeometryExtractionPolicy policy) {
            for (const SceneObjectSnapshot &object : document.objects) {
                if (!object.meshAsset.has_value())
                    continue;
                const auto editorState = ResolveSceneObjectEditorState(document.objects, object.id);
                if (respectEditorVisibility && editorState.has_value() && !editorState->effectivelyVisible)
                    continue;
                const auto result = AppendDocumentAssetMesh(scene, object, assetMeshes, editorState, extracted, policy);
                if (result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        Result<void> ExtractVisibleEntity(const Runtime::RuntimeSceneView &scene, const Runtime::RuntimeEntityView &entity,
                                          const SceneDocumentSnapshot *document, const bool respectEditorVisibility,
                                          const std::span<const Render::IdentifiedRenderLight> canonicalLights,
                                          Runtime::PrimitiveMeshCache &meshCache, EditorViewportSceneSnapshot &snapshot,
                                          const EditorGeometryExtractionPolicy policy) {
            if (entity.authoredObject)
                snapshot.authoredEntities.push_back({entity.entity, SceneObjectId{entity.authoredObject->value}});
            std::optional<ResolvedSceneObjectEditorState> editorState;
            if (document != nullptr && entity.authoredObject.has_value())
                editorState = ResolveSceneObjectEditorState(document->objects, SceneObjectId{entity.authoredObject->value});
            if (respectEditorVisibility && editorState.has_value() && !editorState->effectivelyVisible)
                return Result<void>::Success();
            const auto light = ExtractEntityLight(entity, canonicalLights, snapshot);
            if (light.HasError())
                return light;
            return ExtractPrimitiveMeshInstance(scene, entity, editorState, meshCache, snapshot, policy);
        }

        bool IsVisibleDisabledLight(const SceneObjectSnapshot &object, const SceneDocumentSnapshot &document,
                                    const bool respectEditorVisibility) {
            if (!object.components.light || object.components.light->enabled)
                return false;
            const auto editorState = ResolveSceneObjectEditorState(document.objects, object.id);
            return !respectEditorVisibility || !editorState.has_value() || editorState->effectivelyVisible;
        }

        Result<void> CaptureDisabledLight(const Runtime::RuntimeSceneView &scene, const SceneObjectSnapshot &object,
                                          const SceneDocumentSnapshot &document, const bool respectEditorVisibility,
                                          EditorViewportSceneSnapshot &snapshot) {
            if (!IsVisibleDisabledLight(object, document, respectEditorVisibility))
                return Result<void>::Success();
            if (document.revision != snapshot.documentRevision || document.state.value != scene.DefinitionRevision().value)
                return Result<void>::Failure(MakeError(ViewportSceneErrors::InvalidResult));
            const auto found = std::ranges::lower_bound(snapshot.authoredEntities, object.id, {}, &EditorViewportEntitySource::object);
            if (found == snapshot.authoredEntities.end() || found->object != object.id)
                return Result<void>::Failure(MakeError(ViewportSceneErrors::ObjectNotFound));
            const auto view = scene.Get(found->entity);
            if (view.HasError())
                return Result<void>::Failure(view.ErrorValue());
            if (!view.Value().components->light)
                snapshot.eligibleLights.push_back(
                    {found->entity, object.id, object.components.light, document.revision, scene.DefinitionRevision()});
            return Result<void>::Success();
        }

        Result<void> CaptureDisabledLights(const Runtime::RuntimeSceneView &scene, const SceneDocumentSnapshot *document,
                                           const bool respectEditorVisibility, EditorViewportSceneSnapshot &snapshot) {
            std::ranges::sort(snapshot.authoredEntities, {}, &EditorViewportEntitySource::object);
            if (document == nullptr)
                return Result<void>::Success();
            if (document->objects.size() > 65536)
                return Result<void>::Failure(MakeError(ViewportSceneErrors::InvalidResult));
            for (const auto &object : document->objects) {
                const auto captured = CaptureDisabledLight(scene, object, *document, respectEditorVisibility, snapshot);
                if (captured.HasError())
                    return captured;
            }
            return Result<void>::Success();
        }

        void InitializePreviewStorage(const Runtime::RuntimeSceneView &scene, EditorViewportSceneSnapshot &snapshot) {
            SortExtractedLights(snapshot);
            std::ranges::sort(snapshot.eligibleLights, {}, [](const EditorViewportLightSource &source) {
                return (std::uint64_t{source.entity.entity.generation} << 32U) | source.entity.entity.index;
            });
            snapshot.transformOverrides.resize(scene.SlotCount());
            snapshot.stagedTransformOverrides.resize(scene.SlotCount());
            snapshot.stagedInstanceWorlds.resize(snapshot.instances.size());
            for (auto &bank : snapshot.stagedLights) {
                bank.lights.resize(Render::MaximumSceneLights);
                bank.objects.resize(Render::MaximumSceneLights);
            }
        }
    }  // namespace

    /** @copydoc EditorViewportSceneSnapshot::View */
    EditorViewportSceneView EditorViewportSceneSnapshot::View() const noexcept {
        return EditorViewportSceneView{camera, meshResources,        instances,
                                       lights, runtimeSceneId.value, ComputeEditorViewportLightRevision(lights)};
    }

    /** @copydoc EditorViewportSceneState::Replace */
    void EditorViewportSceneState::Replace(EditorViewportSceneSnapshot snapshot) {
        const bool meshResourcesChanged =
            !std::ranges::equal(m_snapshot.meshResources, snapshot.meshResources, std::ranges::equal_to{},
                                &EditorViewportMeshResourceView::handle, &EditorViewportMeshResourceView::handle);
        m_snapshot = std::move(snapshot);
        if (meshResourcesChanged)
            ++m_meshResourceGeneration;
    }

    /** @copydoc EditorViewportSceneState::Clear */
    void EditorViewportSceneState::Clear() noexcept {
        const bool hadMeshResources = !m_snapshot.meshResources.empty();
        m_snapshot = {};
        if (hadMeshResources)
            ++m_meshResourceGeneration;
    }

    /** @copydoc EditorViewportSceneState::View */
    EditorViewportSceneView EditorViewportSceneState::View() const noexcept {
        return m_snapshot.View();
    }

    /** @copydoc ResolveSceneObjectWorldTransforms */
    Result<SceneObjectWorldTransforms> ResolveSceneObjectWorldTransforms(const Runtime::RuntimeSceneView &scene,
                                                                         const SceneObjectId object) {
        const std::optional<Runtime::EntityRef> entity = scene.Find(Runtime::SceneObjectId{object.value});
        if (!entity)
            return Result<SceneObjectWorldTransforms>::Failure(
                ExtractionError(ViewportSceneErrors::ObjectNotFound, "Runtime scene object does not exist."));
        const Result<Runtime::RuntimeEntityView> value = scene.Get(*entity);
        if (value.HasError())
            return Result<SceneObjectWorldTransforms>::Failure(value.ErrorValue());

        Math::Mat4 parentToWorld = Math::Mat4::Identity();
        if (value.Value().parent) {
            Result<Math::Mat4> parent = ResolveWorld(scene, *value.Value().parent, {}, scene.SlotCount() + 1);
            if (parent.HasError())
                return Result<SceneObjectWorldTransforms>::Failure(parent.ErrorValue());
            parentToWorld = parent.Value();
        }
        const Math::Mat4 localToWorld = Math::Multiply(parentToWorld, value.Value().localTransform->ToMatrix());
        return Result<SceneObjectWorldTransforms>::Success({localToWorld, parentToWorld});
    }

    /** @copydoc ResolveXrTrackingRoot */
    Result<Math::Transform> ResolveXrTrackingRoot(const Runtime::RuntimeSceneView &scene, const Runtime::SceneObjectId object) {
        if (!scene.IsCurrent() || !object.IsValid())
            return Result<Math::Transform>::Failure(
                ExtractionError(ViewportSceneErrors::InvalidResult, "XR tracking root requires its exact live Scene and authored object."));
        const auto resolved = ResolveSceneObjectWorldTransforms(scene, SceneObjectId{object.value});
        if (resolved.HasError())
            return Result<Math::Transform>::Failure(resolved.ErrorValue());
        const auto &matrix = resolved.Value().localToWorld;
        constexpr float tolerance = 0.00001F;
        const Math::Vec3 x{matrix.At(0, 0), matrix.At(1, 0), matrix.At(2, 0)};
        const Math::Vec3 y{matrix.At(0, 1), matrix.At(1, 1), matrix.At(2, 1)};
        if (const Math::Vec3 z{matrix.At(0, 2), matrix.At(1, 2), matrix.At(2, 2)};
            !Math::IsFinite(matrix) || std::abs(Math::Dot(x, x) - 1.0F) > tolerance || std::abs(Math::Dot(y, y) - 1.0F) > tolerance ||
            std::abs(Math::Dot(z, z) - 1.0F) > tolerance || std::abs(Math::Dot(x, y)) > tolerance ||
            std::abs(Math::Dot(x, z)) > tolerance || std::abs(Math::Dot(y, z)) > tolerance ||
            std::abs(Math::Dot(Math::Cross(x, y), z) - 1.0F) > tolerance || std::abs(matrix.At(3, 0)) > tolerance ||
            std::abs(matrix.At(3, 1)) > tolerance || std::abs(matrix.At(3, 2)) > tolerance || std::abs(matrix.At(3, 3) - 1.0F) > tolerance)
            return Result<Math::Transform>::Failure(
                ExtractionError(ViewportSceneErrors::InvalidResult,
                                "XR tracking root must have a finite rigid positive-orientation world transform."));
        auto transform = Math::TryDecomposeAffineTRS(matrix);
        if (transform.HasError())
            return transform;
        auto value = std::move(transform).Value();
        value.scale = {1.0F, 1.0F, 1.0F};
        return Result<Math::Transform>::Success(value);
    }

    /** @copydoc ExtractEditorViewportGeometry */
    Result<EditorViewportSceneSnapshot> ExtractEditorViewportGeometry(
        const Runtime::RuntimeSceneView &scene, const DocumentRevision documentRevision, Runtime::PrimitiveMeshCache &meshCache,
        const SceneDocumentSnapshot *document, const EditorAssetMeshCache *assetMeshes, const bool respectEditorVisibility,
        const EditorGeometryExtractionPolicy policy) {
        EditorViewportSceneSnapshot extracted{.documentRevision = documentRevision, .runtimeSceneId = scene.RuntimeId()};
        std::array<Render::IdentifiedRenderLight, Render::MaximumSceneLights> canonicalLights;
        // Explicit Editor migration interpretation for currently untagged authored lights; this does not prove their historical encoding.
        const Render::SceneColorSourcePolicy sourceColorPolicy{Render::SceneColorEncoding::LegacyLinearRec709D65, 1};
        const auto canonical = Render::ExtractSceneLights(scene, Render::MaximumSceneLights, canonicalLights, sourceColorPolicy);
        if (canonical.HasError())
            return Result<EditorViewportSceneSnapshot>::Failure(canonical.ErrorValue());
        extracted.lightColorSourcePolicy = canonical.Value().sourceColorPolicy;
        const std::size_t capacity = std::min(scene.SlotCount(), policy.maximumInstances);
        extracted.instances.reserve(capacity);
        extracted.instanceObjects.reserve(capacity);
        extracted.instancePickable.reserve(capacity);
        extracted.lights.reserve(canonical.Value().count);
        extracted.lightObjects.reserve(canonical.Value().count);

        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            const std::optional<Runtime::RuntimeEntityView> entity = scene.EntityAt(slot);
            if (!entity)
                continue;
            const auto appended =
                ExtractVisibleEntity(scene, *entity, document, respectEditorVisibility,
                                     std::span{canonicalLights}.first(canonical.Value().count), meshCache, extracted, policy);
            if (appended.HasError())
                return Result<EditorViewportSceneSnapshot>::Failure(appended.ErrorValue());
        }

        if (document != nullptr && assetMeshes != nullptr) {
            if (const Result<void> assetResult =
                    ExtractDocumentAssetMeshes(scene, *document, *assetMeshes, respectEditorVisibility, extracted, policy);
                assetResult.HasError())
                return Result<EditorViewportSceneSnapshot>::Failure(assetResult.ErrorValue());
        }

        const auto disabled = CaptureDisabledLights(scene, document, respectEditorVisibility, extracted);
        if (disabled.HasError())
            return Result<EditorViewportSceneSnapshot>::Failure(disabled.ErrorValue());
        InitializePreviewStorage(scene, extracted);
        return Result<EditorViewportSceneSnapshot>::Success(std::move(extracted));
    }

    /** @copydoc ExtractEditorViewportScene */
    Result<EditorViewportSceneSnapshot> ExtractEditorViewportScene(
        const Runtime::RuntimeSceneView &scene, const DocumentRevision documentRevision, const EditorViewportCamera &camera,
        Runtime::PrimitiveMeshCache &meshCache, const SceneDocumentSnapshot *document, const EditorAssetMeshCache *assetMeshes,
        const bool respectEditorVisibility) {
        if (!camera.IsValid())
            return Result<EditorViewportSceneSnapshot>::Failure(
                ExtractionError(ViewportSceneErrors::InvalidCamera, "Editor viewport camera is invalid."));
        auto extracted = ExtractEditorViewportGeometry(scene, documentRevision, meshCache, document, assetMeshes, respectEditorVisibility);
        if (extracted.HasError())
            return extracted;
        auto snapshot = std::move(extracted).Value();
        snapshot.camera = camera;
        if (!snapshot.View().IsValid())
            return Result<EditorViewportSceneSnapshot>::Failure(
                ExtractionError(ViewportSceneErrors::InvalidResult, "Extracted runtime viewport scene is invalid."));
        return Result<EditorViewportSceneSnapshot>::Success(std::move(snapshot));
    }

}  // namespace Horo::Editor
