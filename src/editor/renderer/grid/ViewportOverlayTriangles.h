#pragma once
/** @file ViewportOverlayTriangles.h
 * @brief Allocation-free conversion of shared viewport overlays to the canonical triangle mesh layout. */
#include "EditorViewportGridGeometry.h"
#include "Horo/Runtime/Render/Mesh.h"

namespace Horo::Editor {
    /** @brief Synchronous borrowed overlay geometry and exact physical-pixel stroke width. */
    struct ViewportOverlayTriangleRequest {
        ViewportGridLineBatch batch;
        Render::RenderCameraView camera;
        Render::FramebufferExtent extent;
        float lineWidthPixels{1.0F};
    };

    /** @brief Copies world triangles or clips and expands line pairs into camera-facing world-space ribbons.
     * @param request Finite shared geometry, camera and physical viewport extent.
     * @param output Caller-owned fixed-capacity canonical vertices; unused entries are degenerate zero vertices.
     * @param count Receives the used triangle vertex count, zero on failure or fully clipped input.
     * @return False for invalid input/capacity/transform; output is cleared on failure. No allocation or resource creation occurs. */
    [[nodiscard]] bool BuildViewportOverlayTriangles(const ViewportOverlayTriangleRequest &request, std::span<Render::MeshVertex> output,
                                                     std::size_t &count) noexcept;
}  // namespace Horo::Editor
