#pragma once

/**
 * @file EditorViewportSceneExtractor.h
 * @brief Immutable conversion from runtime scene views to editor viewport render snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/SceneColorEncoding.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "editor/document/EditorAssetMeshCache.h"
#include "editor/document/SceneDocument.h"
#include "editor/renderer/EditorViewportScene.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Editor {
    /** @brief Exact cold-extraction object identity, including non-light transform parents. */
    struct EditorViewportEntitySource {
        Runtime::EntityRef entity;
        SceneObjectId object;
    };

    /** @brief Exact light provenance; only absent disabled runtime components retain authored values. */
    struct EditorViewportLightSource {
        Runtime::EntityRef entity;
        SceneObjectId object;
        std::optional<Runtime::LightComponent> disabledAuthoredLight;
        DocumentRevision authoredRevision;
        Runtime::SceneDefinitionRevision definitionRevision;
    };

    /** @brief Derived visual transform override indexed by actual entity slot; never authored state. */
    struct EditorViewportTransformOverride {
        Runtime::EntityRef entity;
        Math::Transform localTransform;
        bool present{};
    };

    /** @brief Reusable unpublished light bank. Full cold size preserves capacity when snapshots are copied. */
    struct EditorViewportLightStaging {
        std::vector<Render::RenderLight> lights;
        std::vector<SceneObjectId> objects;

        EditorViewportLightStaging() = default;

        // Scratch contents have no published meaning. Cold snapshot copies restore the full
        // bank size so copying a previously previewed snapshot preserves allocation-free updates.
        EditorViewportLightStaging(const EditorViewportLightStaging &)
            : lights(Render::MaximumSceneLights), objects(Render::MaximumSceneLights) {}

        EditorViewportLightStaging &operator=(const EditorViewportLightStaging &) {
            lights.resize(Render::MaximumSceneLights);
            objects.resize(Render::MaximumSceneLights);
            return *this;
        }

        EditorViewportLightStaging(EditorViewportLightStaging &&) noexcept = default;
        EditorViewportLightStaging &operator=(EditorViewportLightStaging &&) noexcept = default;
    };

    /** @brief Owning viewport render data extracted from one activated runtime scene revision. */
    struct EditorViewportSceneSnapshot {
        DocumentRevision documentRevision;
        Runtime::SceneRuntimeId runtimeSceneId;
        Render::SceneColorSourcePolicy lightColorSourcePolicy; /**< Explicit Editor interpretation retained for all light previews. */
        EditorViewportCamera camera;
        std::vector<Runtime::PrimitiveMeshLease> meshLeases;
        std::vector<EditorAssetMeshLease> importedMeshLeases;
        std::vector<EditorViewportMeshResourceView> meshResources;
        std::vector<EditorViewportInstance> instances;
        std::vector<SceneObjectId> instanceObjects;
        std::vector<std::uint8_t> instancePickable; /**< Parallel 0/1 editor-picking policy for render instances. */
        std::vector<Render::RenderLight> lights;
        std::vector<SceneObjectId> lightObjects;

        // Derived preview metadata and scratch are initialized only by cold extraction.
        std::vector<EditorViewportLightSource> eligibleLights;
        std::vector<EditorViewportEntitySource> authoredEntities; /**< Cold sorted object-to-entity lookup, including non-light parents. */
        std::vector<Runtime::EntityRef> instanceEntities;
        std::vector<EditorViewportTransformOverride> transformOverrides;
        std::vector<EditorViewportTransformOverride> stagedTransformOverrides;
        std::vector<Math::Mat4> stagedInstanceWorlds;
        std::array<EditorViewportLightStaging, 2> stagedLights;
        std::optional<SceneObjectLightPreview> lightOverride;

        [[nodiscard]] EditorViewportSceneView View() const noexcept;
    };

    /** @brief Composition-owned handoff of the active screen's latest immutable viewport snapshot. */
    class EditorViewportSceneState final {
    public:
        void Replace(EditorViewportSceneSnapshot snapshot);
        void Clear() noexcept;
        [[nodiscard]] EditorViewportSceneView View() const noexcept;

        /** @brief Returns the document revision represented by the current handoff snapshot. */
        [[nodiscard]] DocumentRevision Revision() const noexcept {
            return m_snapshot.documentRevision;
        }

        /** @brief Returns the runtime-scene identity represented by the current handoff snapshot. */
        [[nodiscard]] Runtime::SceneRuntimeId RuntimeSceneId() const noexcept {
            return m_snapshot.runtimeSceneId;
        }

        /**
         * @brief Returns the generation changed when mesh-resource identities change.
         *
         * Camera, instance, and light updates preserve this generation because they do not require
         * resource preparation before the latest snapshot can be rendered.
         */
        [[nodiscard]] std::uint64_t MeshResourceGeneration() const noexcept {
            return m_meshResourceGeneration;
        }

    private:
        EditorViewportSceneSnapshot m_snapshot{};
        std::uint64_t m_meshResourceGeneration{};
    };

    /** @brief Resolved object and parent world matrices used by editor manipulation tools. */
    struct SceneObjectWorldTransforms {
        Math::Mat4 localToWorld{Math::Mat4::Identity()};
        Math::Mat4 parentToWorld{Math::Mat4::Identity()};
    };

    /** @brief Resolves world and parent matrices from an immutable runtime scene view.
     * @param scene Borrowed only during this synchronous query; no view or callback escapes.
     * @param object Exact authored object. @return Owned matrices or the original extraction error.
     */
    [[nodiscard]] Result<SceneObjectWorldTransforms> ResolveSceneObjectWorldTransforms(const Runtime::RuntimeSceneView &scene,
                                                                                       SceneObjectId object);

    /** @brief Explicit cold extraction policy; the desktop default preserves its original unbounded scene capacity. */
    struct EditorGeometryExtractionPolicy {
        std::size_t maximumInstances{static_cast<std::size_t>(-1)};
        bool requireImportedMeshes{};
    };

    /** @brief Extracts canonical geometry without selecting or manufacturing a camera.
     * @param scene Exact candidate runtime scene. @param documentRevision Canonical authored revision.
     * @param meshCache Existing primitive owner. @param document Optional authored asset mapping.
     * @param assetMeshes Existing imported owner. @param respectEditorVisibility Existing visibility policy.
     * @param policy Explicit bounded strict admission for selected XR; desktop uses defaults.
     * @return Owned geometry and leases, or atomic extraction failure. Camera remains unset.
     * @details Runtime-only extraction cannot recover disabled components removed by scene conversion.
     * Disabled-light preview eligibility requires the matching document revision and runtime definition
     * state; only owned visual metadata is retained, with exact runtime entity identity.
     */
    [[nodiscard]] Result<EditorViewportSceneSnapshot> ExtractEditorViewportGeometry(
        const Runtime::RuntimeSceneView &scene, DocumentRevision documentRevision, Runtime::PrimitiveMeshCache &meshCache,
        const SceneDocumentSnapshot *document, const EditorAssetMeshCache *assetMeshes, bool respectEditorVisibility,
        EditorGeometryExtractionPolicy policy = {});

    /** @brief Extracts supported renderable instances from an immutable runtime scene view. */
    [[nodiscard]] Result<EditorViewportSceneSnapshot> ExtractEditorViewportScene(
        const Runtime::RuntimeSceneView &scene, DocumentRevision documentRevision, const EditorViewportCamera &camera,
        Runtime::PrimitiveMeshCache &meshCache, const SceneDocumentSnapshot *document = nullptr,
        const EditorAssetMeshCache *assetMeshes = nullptr, bool respectEditorVisibility = true);

    /** @brief Applies editor-owned transient transform overlays without mutating runtime state.
     * @param scene Immutable view borrowed only during this synchronous query.
     * @param previews Existing transient overlays. @param snapshot Owning viewport projection updated in place.
     * @return Success or original validation/extraction failure; no Scene borrow is retained.
     * @details Successful updates retain the component overlay. Failure preserves published instances,
     * lights and applied overlay metadata; only reusable unpublished scratch may change. Cold-extracted
     * snapshot storage is reused without heap allocation on successful preview updates.
     */
    [[nodiscard]] Result<void> ApplyEditorViewportTransformPreview(const Runtime::RuntimeSceneView &scene,
                                                                   std::span<const SceneObjectTransformPreview> previews,
                                                                   EditorViewportSceneSnapshot &snapshot);

    /**
     * @brief Applies or clears one editor-owned transient Light-component overlay.
     * @param scene Active immutable view borrowed only during this synchronous query.
     * @param preview Light override, or null to restore authored runtime values.
     * @param snapshot Owning viewport snapshot updated in place.
     * @return Success, or a typed extraction error without a document mutation.
     * @details Disabled visible sources remain eligible for enable previews. Hidden sources stay excluded.
     * Clearing this overlay preserves the transform overlay. Failure preserves all published values and
     * applied overlay metadata; only reusable unpublished scratch may change.
     */
    [[nodiscard]] Result<void> ApplyEditorViewportLightPreview(const Runtime::RuntimeSceneView &scene,
                                                               const SceneObjectLightPreview *preview,
                                                               EditorViewportSceneSnapshot &snapshot);
}  // namespace Horo::Editor
