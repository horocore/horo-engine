#include "ViewportOverlayTriangles.h"

#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        enum class SegmentVisibility {
            Invalid,
            Clipped,
            Visible
        };

        /** @brief Clips a homogeneous segment to Vulkan's canonical clip volume before perspective division. */
        SegmentVisibility ClipSegment(Math::Vec4 &start, Math::Vec4 &end) noexcept {
            const std::array first{start.w + start.x, start.w - start.x, start.w + start.y, start.w - start.y, start.z, start.w - start.z};
            const std::array last{end.w + end.x, end.w - end.x, end.w + end.y, end.w - end.y, end.z, end.w - end.z};
            float minimum = 0.0F, maximum = 1.0F;
            for (std::size_t plane = 0; plane < first.size(); ++plane) {
                if (!std::isfinite(first[plane]) || !std::isfinite(last[plane]))
                    return SegmentVisibility::Invalid;
                if (first[plane] < 0 && last[plane] < 0)
                    return SegmentVisibility::Clipped;
                if ((first[plane] < 0) != (last[plane] < 0)) {
                    const float crossing = first[plane] / (first[plane] - last[plane]);
                    if (first[plane] < 0)
                        minimum = std::max(minimum, crossing);
                    else
                        maximum = std::min(maximum, crossing);
                }
            }
            if (minimum > maximum)
                return SegmentVisibility::Clipped;
            const auto interpolate = [&](const float fraction) {
                return Math::Vec4{start.x + (end.x - start.x) * fraction, start.y + (end.y - start.y) * fraction,
                                  start.z + (end.z - start.z) * fraction, start.w + (end.w - start.w) * fraction};
            };
            const auto clippedStart = interpolate(minimum);
            end = interpolate(maximum);
            start = clippedStart;
            return start.w > 0 && end.w > 0 ? SegmentVisibility::Visible : SegmentVisibility::Clipped;
        }

        /** @brief Unprojects one pixel-offset endpoint while preserving its original clip depth and homogeneous scale. */
        bool Endpoint(const Math::Mat4 &inverse, Math::Vec4 clip, const Math::Vec2 offset, Math::Vec3 &world) noexcept {
            clip.x += offset.x * clip.w;
            clip.y += offset.y * clip.w;
            const auto value = Math::TransformHomogeneous(inverse, clip);
            if (!std::isfinite(value.w) || std::fabs(value.w) < 0.0000001F)
                return false;
            world = {value.x / value.w, value.y / value.w, value.z / value.w};
            return Math::IsFinite(world);
        }

        /** @brief Expands one visible segment to two triangles with exact endpoint screen-space width. */
        bool AppendLine(Math::Vec4 start, Math::Vec4 end, const Math::Mat4 &inverse, const ViewportOverlayTriangleRequest &request,
                        std::span<Render::MeshVertex> output, std::size_t &count) noexcept {
            const auto visibility = ClipSegment(start, end);
            if (visibility != SegmentVisibility::Visible)
                return visibility == SegmentVisibility::Clipped;
            const Math::Vec2 pixels{(end.x / end.w - start.x / start.w) * request.extent.width,
                                    (end.y / end.w - start.y / start.w) * request.extent.height};
            const float length = std::sqrt(pixels.x * pixels.x + pixels.y * pixels.y);
            if (!std::isfinite(length))
                return false;
            if (length < 0.00001F)
                return true;
            const Math::Vec2 offset{-pixels.y / length * request.lineWidthPixels / request.extent.width,
                                    pixels.x / length * request.lineWidthPixels / request.extent.height};
            std::array<Math::Vec3, 4> corners;
            if (!Endpoint(inverse, start, offset, corners[0]) || !Endpoint(inverse, end, offset, corners[1]) ||
                !Endpoint(inverse, start, {-offset.x, -offset.y}, corners[2]) ||
                !Endpoint(inverse, end, {-offset.x, -offset.y}, corners[3]))
                return false;
            for (const auto index : std::array{0U, 1U, 2U, 2U, 1U, 3U})
                output[count++] = {corners[index], {0, 1, 0}, {}};
            return true;
        }
    }  // namespace

    /** @copydoc BuildViewportOverlayTriangles */
    bool BuildViewportOverlayTriangles(const ViewportOverlayTriangleRequest &request, const std::span<Render::MeshVertex> output,
                                       std::size_t &count) noexcept {
        std::ranges::fill(output, Render::MeshVertex{});
        count = 0;
        const auto &batch = request.batch;
        const bool lines = batch.topology == ViewportGridPrimitiveTopology::Lines;
        const auto factor = lines ? 3U : 1U;
        if (!batch.IsValid() || !request.camera.IsValid() || !request.extent.IsValid() || !std::isfinite(request.lineWidthPixels) ||
            request.lineWidthPixels <= 0 || batch.positions.size() > output.size() / factor)
            return false;
        if (!lines) {
            for (const auto position : batch.positions)
                output[count++] = {position, {0, 1, 0}, {}};
            return true;
        }
        const auto matrix =
            BuildRenderMvp(request.camera, Math::Mat4::Identity(), static_cast<float>(request.extent.width) / request.extent.height,
                           Math::ClipDepthRange::ZeroToOne);
        if (matrix.HasError())
            return false;
        const auto inverse = Math::TryInverse(matrix.Value());
        if (inverse.HasError())
            return false;
        for (std::size_t index = 0; index < batch.positions.size(); index += 2) {
            const auto first = batch.positions[index], last = batch.positions[index + 1];
            if (!AppendLine(Math::TransformHomogeneous(matrix.Value(), {first.x, first.y, first.z, 1}),
                            Math::TransformHomogeneous(matrix.Value(), {last.x, last.y, last.z, 1}), inverse.Value(), request, output,
                            count)) {
                std::ranges::fill(output, Render::MeshVertex{});
                count = 0;
                return false;
            }
        }
        return true;
    }
}  // namespace Horo::Editor
