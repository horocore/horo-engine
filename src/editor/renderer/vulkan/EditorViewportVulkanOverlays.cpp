#include "EditorViewportRendererVulkan.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/EditorViewportDisplayColor.h"
#include "editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        /** @brief Appends one shared batch and returns its explicit used vertex count. */
        Result<std::size_t> AppendOverlayBatch(const ViewportOverlayTriangleRequest &request, const std::span<Render::MeshVertex> output,
                                               const std::size_t color, const std::span<float, 12> palette) {
            std::size_t added{};
            if (!BuildViewportOverlayTriangles(request, output, added))
                return Result<std::size_t>::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            for (auto &vertex : output.first(added))
                vertex.uv = {static_cast<float>(color), 0};
            palette[color * 4] = request.batch.color.x;
            palette[color * 4 + 1] = request.batch.color.y;
            palette[color * 4 + 2] = request.batch.color.z;
            palette[color * 4 + 3] = 1;
            return Result<std::size_t>::Success(added);
        }
    }  // namespace

    /** @copydoc EditorViewportRendererVulkan::WriteOverlayUniforms */
    Result<void> EditorViewportRendererVulkan::WriteOverlayUniforms(const Render::FrameToken frame, const OverlaySlot &slot,
                                                                    const Render::RenderCameraView &camera,
                                                                    const Render::FramebufferExtent extent,
                                                                    const std::span<const float> palette) {
        const auto matrix = BuildRenderMvp(camera, Math::Mat4::Identity(), static_cast<float>(extent.width) / extent.height,
                                           Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return Result<void>::Failure(matrix.ErrorValue());
        auto mvp = matrix.Value();
        for (std::size_t column = 0; column < 4; ++column)
            mvp.values[column * 4 + 1] *= -1;
        const auto identity = Math::Mat4::Identity();
        const std::array mode{0.0F, 0.0F, 0.0F, -1.0F};
        std::array<std::byte, MaximumUniformBytes> storage{};
        const auto bytes = std::span{storage}.first(shader_.uniformBytes);
        for (const auto parameter : {1U, 2U, 3U}) {
            const auto written = ProgramResources::WriteParameter(shader_.layout, bytes, {parameter},
                                                                  parameter == 1 ? std::span<const float>{mvp.values}
                                                                                 : std::span<const float>{identity.values});
            if (written.HasError())
                return written;
        }
        if (const auto written = ProgramResources::WriteParameter(shader_.layout, bytes, {4}, mode); written.HasError())
            return written;
        if (const auto written = ProgramResources::WriteParameter(shader_.layout, bytes, {10}, palette); written.HasError())
            return written;
        return bridge_.Port()->WriteFrameBuffer(frame, slot.uniforms.instance, bytes);
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareOverlay */
    Result<bool> EditorViewportRendererVulkan::PrepareOverlay(const Render::FrameToken frame, OverlaySlot &slot,
                                                              const Render::StaticMeshPassDescriptor &pass) {
        using Outcome = Result<bool>;
        std::size_t count{};
        std::array<float, 12> palette{};
        if (requestedGrid_.visible) {
            ViewportGridGeometry grid;
            if (!BuildViewportGridGeometry({pass.scene.camera, static_cast<float>(pass.extent.width) / pass.extent.height,
                                            static_cast<float>(pass.extent.height), requestedGrid_.targetMinorSpacingPixels,
                                            requestedGrid_.targetLineWidthPixels},
                                           grid))
                return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            const std::array batches{grid.RegularLines(), grid.Axes()};
            for (std::size_t color = 0; color < batches.size(); ++color) {
                const auto added =
                    AppendOverlayBatch({batches[color], pass.scene.camera, pass.extent, requestedGrid_.targetLineWidthPixels},
                                       std::span{overlayVertices_}.subspan(count), color, palette);
                if (added.HasError())
                    return Outcome::Failure(added.ErrorValue());
                count += added.Value();
            }
        }
        if (requestedLightVisualizer_.selectedLight) {
            LightVisualizerGeometry light;
            if (!BuildLightVisualizerGeometry({pass.scene.camera, *requestedLightVisualizer_.selectedLight}, light))
                return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
            const auto added = AppendOverlayBatch({{light.Lines(), light.color, ViewportGridPrimitiveTopology::Lines},
                                                   pass.scene.camera,
                                                   pass.extent,
                                                   1.0F},
                                                  std::span{overlayVertices_}.subspan(count), 2, palette);
            if (added.HasError())
                return Outcome::Failure(added.ErrorValue());
            count += added.Value();
        }
        if (count == 0)
            return Outcome::Success(false);
        // SYNC: The selected runtime checks canonical frame admission and exact prior buffer retirement before either write.
        if (const auto written = bridge_.Port()->WriteFrameBuffer(frame, slot.vertexInstance, std::as_bytes(std::span{overlayVertices_}));
            written.HasError())
            return Outcome::Failure(written.ErrorValue());
        if (color_) {
            for (std::size_t index = 0; index < 8; index += 4) {
                const auto decoded = DecodeEditorOverlaySrgbV1({palette[index], palette[index + 1], palette[index + 2]});
                if (decoded.HasError())
                    return Outcome::Failure(decoded.ErrorValue());
                palette[index] = decoded.Value().x;
                palette[index + 1] = decoded.Value().y;
                palette[index + 2] = decoded.Value().z;
            }
            if (requestedLightVisualizer_.selectedLight) {
                const auto display = LightOverlayDisplayLinearV1(requestedLightVisualizer_.selectedLight->color);
                if (display.HasError())
                    return Outcome::Failure(display.ErrorValue());
                palette[8] = std::clamp(display.Value().x, 0.35F, 1.0F);
                palette[9] = std::clamp(display.Value().y, 0.35F, 1.0F);
                palette[10] = std::clamp(display.Value().z, 0.20F, 1.0F);
            }
        }
        if (const auto written = WriteOverlayUniforms(frame, slot, pass.scene.camera, pass.extent, palette); written.HasError())
            return Outcome::Failure(written.ErrorValue());
        return Outcome::Success(true);
    }
}  // namespace Horo::Editor
