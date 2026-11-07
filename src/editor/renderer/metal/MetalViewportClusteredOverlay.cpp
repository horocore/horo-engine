#include "EditorViewportDisplayColor.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "MetalViewportClusteredFrame.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        std::array<float, 6> ClipDistances(const Math::Vec4 point) noexcept {
            return {point.w + point.x, point.w - point.x, point.w + point.y, point.w - point.y, point.z, point.w - point.z};
        }

        Math::Vec4 InterpolateClip(const Math::Vec4 a, const Math::Vec4 b, const float t) noexcept {
            return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        }

        bool ClipLine(Math::Vec4 &a, Math::Vec4 &b) noexcept {
            const auto start = ClipDistances(a), end = ClipDistances(b);
            float minimum{}, maximum{1};
            for (std::size_t plane = 0; plane < start.size(); ++plane) {
                if (start[plane] < 0 && end[plane] < 0)
                    return false;
                if (start[plane] < 0)
                    minimum = std::max(minimum, start[plane] / (start[plane] - end[plane]));
                if (end[plane] < 0)
                    maximum = std::min(maximum, start[plane] / (start[plane] - end[plane]));
            }
            const auto original = a;
            a = InterpolateClip(original, b, minimum);
            b = InterpolateClip(original, b, maximum);
            return minimum <= maximum && a.w > Math::DefaultEpsilon && b.w > Math::DefaultEpsilon;
        }

        Result<void> ExpandLines(const std::span<const Math::Vec3> lines, const Math::Mat4 &matrix, const Render::FramebufferExtent extent,
                                 const std::span<Render::MeshVertex> vertices) {
            if (lines.size() % 2 != 0 || lines.size() / 2 > vertices.size() / 6)
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::Capacity));
            std::size_t output{};
            for (std::size_t index = 0; index < lines.size(); index += 2) {
                const auto start = lines[index], end = lines[index + 1];
                auto a = Math::TransformHomogeneous(matrix, {start.x, start.y, start.z, 1});
                auto b = Math::TransformHomogeneous(matrix, {end.x, end.y, end.z, 1});
                if (!Math::IsFinite(a) || !Math::IsFinite(b))
                    return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
                if (!ClipLine(a, b))
                    continue;
                const Math::Vec3 first{a.x / a.w, a.y / a.w, a.z / a.w}, last{b.x / b.w, b.y / b.w, b.z / b.w};
                const float dx = (last.x - first.x) * extent.width, dy = (last.y - first.y) * extent.height;
                const float length = std::hypot(dx, dy);
                if (length <= Math::DefaultEpsilon)
                    continue;
                const Math::Vec3 width{-dy / (length * extent.width), dx / (length * extent.height), 0};
                const std::array positions{first - width, last - width, first + width, first + width, last - width, last + width};
                for (const auto position : positions)
                    vertices[output++].position = position;
            }
            return Result<void>::Success();
        }
    }  // namespace

    void MetalViewportClusteredFrame::SetPresentation(const EditorViewportGridOptions &grid,
                                                      const EditorViewportLightVisualizerOptions &visualizer) noexcept {
        gridOptions_ = grid;
        visualizerOptions_ = visualizer;
    }

    Result<void> MetalViewportClusteredFrame::PrepareOverlays(Slot &slot) {
        using namespace Render;
        constexpr std::array<std::size_t, 3> counts{ViewportGridGeometry::MaxRegularVertices / 3 * 3, 12,
                                                    LightVisualizerGeometry::MaximumVertexCount * 3};
        for (std::size_t index = 0; index < slot.overlays.size(); ++index)
            if (const auto prepared = PrepareOverlay(slot.overlays[index], counts[index]); prepared.HasError())
                return prepared;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::PrepareOverlay(Slot::Overlay &overlay, const std::size_t count) {
        using namespace Render;
        if (overlay.mesh.IsValid())
            return Result<void>::Success();
        overlay.vertices.resize(count);
        overlay.indices.resize(count);
        for (std::size_t vertex = 0; vertex < count; ++vertex)
            overlay.indices[vertex] = static_cast<std::uint32_t>(vertex);
        if (!overlay.descriptor.vertexBuffer.IsValid()) {
            const auto buffer = frontend_->CreateBuffer({.byteSize = count * sizeof(MeshVertex),
                                                         .usage = RenderBufferUsage::Vertex,
                                                         .access = RenderBufferAccess::HostVisible},
                                                        {});
            if (buffer.HasError())
                return Result<void>::Failure(buffer.ErrorValue());
            overlay.descriptor.vertexBuffer = buffer.Value().handle;
        }
        if (!overlay.descriptor.indexBuffer.IsValid()) {
            const auto buffer = frontend_->CreateBuffer({.byteSize = count * sizeof(std::uint32_t),
                                                         .usage = RenderBufferUsage::Index,
                                                         .access = RenderBufferAccess::HostVisible},
                                                        std::as_bytes(std::span{overlay.indices}));
            if (buffer.HasError())
                return Result<void>::Failure(buffer.ErrorValue());
            overlay.descriptor.indexBuffer = buffer.Value().handle;
        }
        if (const auto processed = frontend_->ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        overlay.descriptor.vertexStride = sizeof(MeshVertex);
        overlay.descriptor.vertexCount = static_cast<std::uint32_t>(count);
        overlay.descriptor.indexCount = static_cast<std::uint32_t>(count);
        overlay.descriptor.localBounds = {{-1, -1, -1}, {1, 1, 1}};
        const auto mesh = frontend_->CreateMesh(overlay.descriptor);
        if (mesh.HasError())
            return Result<void>::Failure(mesh.ErrorValue());
        overlay.mesh = mesh.Value().handle;
        if (const auto processed = frontend_->ProcessResourceRequests(); processed.HasError())
            return Result<void>::Failure(processed.ErrorValue());
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::WriteGridOverlay(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor,
                                                               const Math::Mat4 &matrix) {
        if (!gridOptions_.visible)
            return Result<void>::Success();
        const auto aspect = static_cast<float>(descriptor.extent.width) / descriptor.extent.height;

        if (!BuildViewportGridGeometry({descriptor.scene.camera, aspect, static_cast<float>(descriptor.extent.height),
                                        gridOptions_.targetMinorSpacingPixels, gridOptions_.targetLineWidthPixels},
                                       gridGeometry_))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        const auto regular = gridGeometry_.RegularLines();
        for (std::size_t index = 0; index < regular.positions.size(); ++index)
            slot.overlays[0].vertices[index].position = regular.positions[index];
        slot.overlays[0].color = regular.color;
        slot.overlays[1].color = gridGeometry_.Axes().color;
        if (const auto axes = ExpandLines(gridGeometry_.Axes().positions, matrix, descriptor.extent, slot.overlays[1].vertices);
            axes.HasError())
            return axes;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::WriteVisualizerOverlay(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor,
                                                                     const Math::Mat4 &matrix) {
        if (!visualizerOptions_.selectedLight)
            return Result<void>::Success();

        if (!BuildLightVisualizerGeometry({descriptor.scene.camera, *visualizerOptions_.selectedLight}, visualizerGeometry_))
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
        slot.overlays[2].color = visualizerGeometry_.color;
        if (color_) {
            const auto display = LightOverlayDisplayLinearV1(visualizerOptions_.selectedLight->color);
            if (display.HasError())
                return Result<void>::Failure(display.ErrorValue());
            slot.overlays[2].color = {std::clamp(display.Value().x, 0.35F, 1.0F), std::clamp(display.Value().y, 0.35F, 1.0F),
                                      std::clamp(display.Value().z, 0.20F, 1.0F)};
        }
        if (const auto lines = ExpandLines(visualizerGeometry_.Lines(), matrix, descriptor.extent, slot.overlays[2].vertices);
            lines.HasError())
            return lines;
        return Result<void>::Success();
    }

    Result<void> MetalViewportClusteredFrame::WriteOverlays(Slot &slot, const Render::StaticMeshPassDescriptor &descriptor) {
        const auto aspect = static_cast<float>(descriptor.extent.width) / descriptor.extent.height;
        const auto matrix = BuildRenderMvp(descriptor.scene.camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return Result<void>::Failure(matrix.ErrorValue());
        for (auto &overlay : slot.overlays)
            std::ranges::fill(overlay.vertices, Render::MeshVertex{});
        if (const auto grid = WriteGridOverlay(slot, descriptor, matrix.Value()); grid.HasError())
            return grid;
        if (const auto visualizer = WriteVisualizerOverlay(slot, descriptor, matrix.Value()); visualizer.HasError())
            return visualizer;
        slot.overlays[0].matrix = matrix.Value();
        for (std::size_t index = 0; index < slot.overlays.size(); ++index) {
            auto &overlay = slot.overlays[index];
            auto &uniform = slot.uniforms[6 * MaximumDraws + index];
            auto color = Result<Math::Vec3>::Success(overlay.color);
            if (color_ && index < 2)
                color = DecodeEditorOverlaySrgbV1(overlay.color);
            if (color.HasError())
                return Result<void>::Failure(color.ErrorValue());
            const auto packed = PackEditorViewportGrid(programs_->Role(1).layout, overlay.matrix, color.Value(), uniform.bytes);
            if (packed.HasError())
                return Result<void>::Failure(packed.ErrorValue());
            overlayUpdates_[index * 2] = {overlay.descriptor.vertexBuffer, std::as_bytes(std::span{overlay.vertices}), 0};
            overlayUpdates_[index * 2 + 1] = {uniform.buffer, std::span{uniform.bytes}.first(programs_->Role(1).uniformBytes),
                                              uniform.offset};
        }
        return MetalViewportResourceBridge::UpdateBuffers(*frontend_, *bridge_, overlayUpdates_);
    }
}  // namespace Horo::Editor
