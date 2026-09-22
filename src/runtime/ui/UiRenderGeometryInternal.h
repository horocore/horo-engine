#pragma once

#include "Horo/Runtime/Ui/UiRenderGeometry.h"

#include <array>
#include <cstdint>
#include <vector>

namespace Horo::Runtime::Ui::RenderGeometryInternal {
    /** @brief Describes one transformed indexed quad appended to a geometry plan. */
    struct QuadDescriptor final {
        const UiLogicalTransform *transform{};
        std::array<float, 2> origin;
        std::array<float, 2> extent;
        std::array<float, 4> uv;
        UiLinearColor color;
    };

    /**
     * @brief Appends one transformed indexed quad.
     * @param vertices Destination vertex storage.
     * @param indices Destination index storage.
     * @param quad Quad geometry and paint data.
     */
    void AppendQuad(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const QuadDescriptor &quad);

    /**
     * @brief Appends the visible border quads for one logical rectangle.
     * @param vertices Destination vertex storage.
     * @param indices Destination index storage.
     * @param transform Transform applied to each generated vertex.
     * @param rect Destination rectangle.
     * @param width Border width in logical units.
     * @param color Border color after command opacity.
     */
    void AppendBorder(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const UiLogicalTransform &transform,
                      UiLogicalRect rect, std::int32_t width, UiLinearColor color);

    /**
     * @brief Appends nine indexed quads for one nine-slice image.
     * @param vertices Destination vertex storage.
     * @param indices Destination index storage.
     * @param transform Transform applied to each generated vertex.
     * @param rect Destination rectangle.
     * @param draw Nine-slice source and border data.
     * @param color Tint after command opacity.
     */
    void AppendNineSlice(std::vector<UiRenderVertex> &vertices, std::vector<std::uint32_t> &indices, const UiLogicalTransform &transform,
                         UiLogicalRect rect, const UiNineSliceDraw &draw, UiLinearColor color);
}  // namespace Horo::Runtime::Ui::RenderGeometryInternal
