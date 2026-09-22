#include "UiRenderGeometryInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Runtime::Ui::RenderGeometryInternal {
    namespace {
        struct FloatPoint final {
            float x{};
            float y{};
        };

        [[nodiscard]] FloatPoint TransformPoint(const UiLogicalTransform &transform, const float x, const float y) noexcept {
            const auto &m = transform.values;
            return {m[0] * x + m[1] * y + m[4], m[2] * x + m[3] * y + m[5]};
        }

        [[nodiscard]] std::array<FloatPoint, 4> RectangleCorners(const float x, const float y, const float width,
                                                                 const float height) noexcept {
            return {FloatPoint{x, y}, FloatPoint{x + width, y}, FloatPoint{x + width, y + height}, FloatPoint{x, y + height}};
        }
    }  // namespace

    void AppendQuad(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const QuadDescriptor &quad) {
        const auto corners = RectangleCorners(quad.origin[0], quad.origin[1], quad.extent[0], quad.extent[1]);
        const std::array<FloatPoint, 4> transformed{
            TransformPoint(*quad.transform, corners[0].x, corners[0].y),
            TransformPoint(*quad.transform, corners[1].x, corners[1].y),
            TransformPoint(*quad.transform, corners[2].x, corners[2].y),
            TransformPoint(*quad.transform, corners[3].x, corners[3].y),
        };
        const auto first = static_cast<std::uint32_t>(vertices.size());
        vertices.push_back({transformed[0].x, transformed[0].y, quad.uv[0], quad.uv[1], quad.color});
        vertices.push_back({transformed[1].x, transformed[1].y, quad.uv[2], quad.uv[1], quad.color});
        vertices.push_back({transformed[2].x, transformed[2].y, quad.uv[2], quad.uv[3], quad.color});
        vertices.push_back({transformed[3].x, transformed[3].y, quad.uv[0], quad.uv[3], quad.color});
        indices.insert(indices.end(), {first, first + 1U, first + 2U, first, first + 2U, first + 3U});
    }

    void AppendBorder(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const UiLogicalTransform &transform,
                      const UiLogicalRect rect, const std::int32_t width, const UiLinearColor color) {
        const float x = static_cast<float>(rect.origin.x);
        const float y = static_cast<float>(rect.origin.y);
        const float extentX = static_cast<float>(rect.extent.width);
        const float extentY = static_cast<float>(rect.extent.height);
        const float border = static_cast<float>(width);
        if (border <= 0.0F || extentX <= 0.0F || extentY <= 0.0F)
            return;

        const float horizontal = std::min(extentY * 0.5F, border);
        const float vertical = std::min(extentX * 0.5F, border);
        const float innerHeight = std::max(0.0F, extentY - 2.0F * horizontal);
        const std::array<float, 4> uv{0.0F, 0.0F, 0.0F, 0.0F};
        AppendQuad(vertices, indices, {&transform, {x, y}, {extentX, horizontal}, uv, color});
        AppendQuad(vertices, indices, {&transform, {x, y + extentY - horizontal}, {extentX, horizontal}, uv, color});
        AppendQuad(vertices, indices, {&transform, {x, y + horizontal}, {vertical, innerHeight}, uv, color});
        AppendQuad(vertices, indices, {&transform, {x + extentX - vertical, y + horizontal}, {vertical, innerHeight}, uv, color});
    }

    void AppendNineSlice(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const UiLogicalTransform &transform,
                         const UiLogicalRect rect, const UiNineSliceDraw &draw, const UiLinearColor color) {
        const float x = static_cast<float>(rect.origin.x);
        const float y = static_cast<float>(rect.origin.y);
        const float width = static_cast<float>(rect.extent.width);
        const float height = static_cast<float>(rect.extent.height);
        const float sourceWidth = static_cast<float>(draw.sourceExtent.width);
        const float sourceHeight = static_cast<float>(draw.sourceExtent.height);
        const float uvWidth = draw.uv[2] - draw.uv[0];
        const float uvHeight = draw.uv[3] - draw.uv[1];
        const float leftFraction = static_cast<float>(draw.insets.left) / sourceWidth;
        const float rightFraction = static_cast<float>(draw.insets.right) / sourceWidth;
        const float topFraction = static_cast<float>(draw.insets.top) / sourceHeight;
        const float bottomFraction = static_cast<float>(draw.insets.bottom) / sourceHeight;
        const float leftInset = static_cast<float>(draw.insets.left);
        const float rightInset = static_cast<float>(draw.insets.right);
        const float topInset = static_cast<float>(draw.insets.top);
        const float bottomInset = static_cast<float>(draw.insets.bottom);
        const float horizontalInsets = leftInset + rightInset;
        const float verticalInsets = topInset + bottomInset;
        const float horizontalScale = horizontalInsets > width ? width / horizontalInsets : 1.0F;
        const float verticalScale = verticalInsets > height ? height / verticalInsets : 1.0F;
        const std::array<float, 4> xPositions{x, x + leftInset * horizontalScale, x + width - rightInset * horizontalScale, x + width};
        const std::array<float, 4> yPositions{y, y + topInset * verticalScale, y + height - bottomInset * verticalScale, y + height};
        const std::array<float, 4> uCoordinates{draw.uv[0], draw.uv[0] + uvWidth * leftFraction, draw.uv[2] - uvWidth * rightFraction,
                                                draw.uv[2]};
        const std::array<float, 4> vCoordinates{draw.uv[1], draw.uv[1] + uvHeight * topFraction, draw.uv[3] - uvHeight * bottomFraction,
                                                draw.uv[3]};
        for (std::size_t row = 0; row < 3; ++row)
            for (std::size_t column = 0; column < 3; ++column)
                AppendQuad(vertices, indices,
                           {&transform,
                            {xPositions[column], yPositions[row]},
                            {xPositions[column + 1] - xPositions[column], yPositions[row + 1] - yPositions[row]},
                            {uCoordinates[column], vCoordinates[row], uCoordinates[column + 1], vCoordinates[row + 1]},
                            color});
    }
}  // namespace Horo::Runtime::Ui::RenderGeometryInternal
