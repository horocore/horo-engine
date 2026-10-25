#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>
#include <numeric>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::CreateOverlaySlots */
    Result<void> EditorViewportRendererVulkan::CreateOverlaySlots() {
        for (std::uint32_t index = 0; index < device_.frames; ++index) {
            auto &slot = slots_[index].overlay;
            if (const auto mesh = CreateOverlayMesh(slot); mesh.HasError())
                return mesh;
            if (const auto uniform =
                    CreateUniformSlot(slot.uniforms, (index + 1) * (Render::RenderMaterialPass::MaximumDraws + 1), shader_.overlayPipeline);
                uniform.HasError())
                return uniform;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::CreateOverlayMesh */
    Result<void> EditorViewportRendererVulkan::CreateOverlayMesh(OverlaySlot &slot) {
        using namespace Render;
        std::ranges::fill(overlayVertices_, MeshVertex{});
        const auto vertex = frontend_.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                   {sizeof(overlayVertices_), RenderBufferUsage::Vertex, RenderBufferAccess::HostVisible},
                                                   std::as_bytes(std::span{overlayVertices_}));
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        slot.vertices = vertex.Value().handle;
        std::array<std::uint32_t, MaximumOverlayVertices> indices;
        std::iota(indices.begin(), indices.end(), 0U);
        const auto index = frontend_.CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                  {sizeof(indices), RenderBufferUsage::Index, RenderBufferAccess::HostVisible},
                                                  std::as_bytes(std::span{indices}));
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        slot.indices = index.Value().handle;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        for (const auto operation : {vertex.Value().operation, index.Value().operation})
            if (const auto ready = frontend_.ResourceOperationResult(operation); ready.HasError())
                return ready;
        const auto native = Detail::RenderFrontendResourceAccess::BackendInstance(frontend_, slot.vertices);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        slot.vertexInstance = native.Value();
        // These preallocated host-written vertices are directly submitted, never culled through scene extraction.
        const auto mesh = frontend_.CreateMesh({slot.vertices,
                                                slot.indices,
                                                sizeof(MeshVertex),
                                                MaximumOverlayVertices,
                                                RenderIndexFormat::UInt32,
                                                MaximumOverlayVertices,
                                                RenderPrimitiveTopology::Triangles,
                                                {{-1.0e20F, -1.0e20F, -1.0e20F}, {1.0e20F, 1.0e20F, 1.0e20F}}});
        if (mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        slot.mesh = mesh.Value().handle;
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        return frontend_.ResourceOperationResult(mesh.Value().operation);
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseOverlaySlots */
    void EditorViewportRendererVulkan::ReleaseOverlaySlots() noexcept {
        for (auto &frame : slots_) {
            auto &slot = frame.overlay;
            slot.uniforms.material.reset();
            if (slot.mesh.IsValid())
                static_cast<void>(frontend_.ReleaseMesh(slot.mesh));
            for (const auto buffer : {slot.uniforms.buffer, slot.vertices, slot.indices})
                if (buffer.IsValid())
                    static_cast<void>(frontend_.ReleaseBuffer(buffer));
            slot = {};
        }
    }
}  // namespace Horo::Editor
