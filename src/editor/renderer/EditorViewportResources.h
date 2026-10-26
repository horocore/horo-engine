#pragma once

#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorViewportRenderer.h"

#include <optional>
#include <unordered_map>

namespace Horo::Editor {
    /** @brief Resolves a ready typed texture view into the selected GUI backend's image identity. */
    using EditorViewportImageResolver = Result<std::uintptr_t> (*)(const Render::RenderFrontend &, Render::RenderTextureViewHandle);

    /** @brief Backend-specific attachment policy used by the shared viewport resource coordinator. */
    struct EditorViewportResourceConfig {
        Render::RenderMemoryScopeId memoryScope{RenderMemoryScopes::ViewportResources};
        Render::RenderTextureFormat depthFormat{Render::RenderTextureFormat::Depth32Float};
        Render::RenderTextureAspect depthAspect{Render::RenderTextureAspect::Depth};
        EditorViewportImageResolver resolveImage{nullptr};
        Render::RenderBufferAccess meshBufferAccess{Render::RenderBufferAccess::DeviceLocal};
        bool prepareDirectionalShadow{true}; /**< Allocate only attachments actually consumed by the selected viewport recipe. */
    };

    /** @brief Owns frontend-backed viewport resources and atomic replacement state. */
    class EditorViewportResources final {
    public:
        struct MeshBinding {
            Render::RenderMeshHandle mesh;
            std::uint32_t indexCount{0};
            Render::RenderMeshDescriptor descriptor; /**< Exact ready frontend recipe, including its actual vertex/index generations. */
        };

        EditorViewportResources(Render::RenderFrontend &frontend, const EditorViewportResourceConfig &config) noexcept;

        [[nodiscard]] Result<std::optional<Render::RenderTargetHandle>> Prepare(const Render::RenderSceneView &scene,
                                                                                EditorViewportExtent requestedExtent);
        /** @brief Prepares the SAME mesh owner without allocating an unrelated desktop camera target.
         * @param resources Canonical immutable mesh projection retained by the scene owner.
         * @return Success or original resource/upload failure; pending readiness remains observable through FindMesh.
         * @pre Lifecycle preparation only, before graph topology admission; normal frames never synchronize meshes.
         */
        [[nodiscard]] Result<void> PrepareMeshes(std::span<const EditorViewportMeshResourceView> resources);
        void Shutdown() noexcept;

        [[nodiscard]] std::optional<MeshBinding> FindMesh(std::uint64_t sourceId) const noexcept;
        [[nodiscard]] Render::RenderTargetHandle Target() const noexcept;
        [[nodiscard]] Render::RenderTargetHandle ShadowTarget() const noexcept;
        [[nodiscard]] Render::RenderTextureViewHandle ShadowTextureView() const noexcept;
        /** @brief Exact current color view borrowed by the matching GUI composition. */
        [[nodiscard]] Render::RenderTextureViewHandle ColorTextureView() const noexcept;
        /** @brief Exact existing color storage, borrowed to compile a clear recipe at its resource boundary. */
        [[nodiscard]] Render::RenderTextureHandle ColorTexture() const noexcept;
        [[nodiscard]] EditorViewportExtent AllocatedExtent() const noexcept;
        [[nodiscard]] std::uintptr_t ImageIdentity() const noexcept;
        [[nodiscard]] bool IsReady() const noexcept;

    private:
        struct ResidentMesh {
            Render::RenderBufferHandle vertexBuffer;
            Render::RenderBufferHandle indexBuffer;
            Render::RenderMeshHandle mesh;
            Render::ResourceOperationId vertexOperation;
            Render::ResourceOperationId indexOperation;
            Render::ResourceOperationId meshOperation;
            std::uint32_t indexCount{0};
            std::uint32_t sourceGeneration{0};
            Render::RenderMeshDescriptor descriptor;
        };

        struct ViewportTargetResources {
            Render::RenderTextureHandle colorTexture;
            Render::RenderTextureHandle depthTexture;
            Render::RenderTextureViewHandle colorView;
            Render::RenderTextureViewHandle depthView;
            Render::RenderTargetHandle target;
            Render::ResourceOperationId colorTextureOperation;
            Render::ResourceOperationId depthTextureOperation;
            Render::ResourceOperationId colorViewOperation;
            Render::ResourceOperationId depthViewOperation;
            Render::ResourceOperationId targetOperation;
            EditorViewportExtent extent;
            std::uintptr_t imageIdentity{0};
        };

        [[nodiscard]] Result<void> SynchronizeMeshes(std::span<const EditorViewportMeshResourceView> resources);
        [[nodiscard]] Result<bool> SynchronizeMesh(const EditorViewportMeshResourceView &resource);
        [[nodiscard]] Result<bool> AdvanceResidentMesh(const EditorViewportMeshResourceView &resource, ResidentMesh &resident);
        [[nodiscard]] Result<void> CreateResidentMeshBuffers(const EditorViewportMeshResourceView &resource, ResidentMesh &resident);
        [[nodiscard]] bool IsResidentMeshReady(const ResidentMesh &resident) const;
        void RetireMissingMeshes(std::span<const EditorViewportMeshResourceView> resources) noexcept;
        [[nodiscard]] Result<void> SynchronizeTarget(EditorViewportExtent extent);
        [[nodiscard]] Result<bool> AdvanceViewportTarget(ViewportTargetResources &resources);
        [[nodiscard]] Result<bool> AdvanceViewportTextures(ViewportTargetResources &resources);
        [[nodiscard]] Result<bool> AdvanceViewportViews(ViewportTargetResources &resources);
        [[nodiscard]] Result<void> SynchronizeShadowResources();
        [[nodiscard]] Result<bool> AdvanceShadowTarget();
        void ReleaseMesh(ResidentMesh &mesh) noexcept;
        void ReleaseShadowResources() noexcept;
        void ReleaseViewportTarget(ViewportTargetResources &resources) noexcept;

        Render::RenderFrontend *frontend_{nullptr};
        EditorViewportResourceConfig config_;
        std::unordered_map<std::uint64_t, ResidentMesh> meshes_;
        std::unordered_map<std::uint64_t, ResidentMesh> pendingMeshes_;
        ViewportTargetResources viewportTarget_;
        ViewportTargetResources pendingViewportTarget_;
        Render::RenderTextureHandle shadowDepthTexture_;
        Render::RenderTextureViewHandle shadowDepthTextureView_;
        Render::RenderTargetHandle shadowTarget_;
        Render::ResourceOperationId shadowDepthTextureOperation_;
        Render::ResourceOperationId shadowDepthTextureViewOperation_;
        Render::ResourceOperationId shadowTargetOperation_;
        EditorViewportExtent allocatedExtent_;
        bool meshesReady_{false};
    };
}  // namespace Horo::Editor
