#pragma once
/** @file EditorViewportMatrixComponents.h @brief Private conversion from scene matrix storage to typed material components. */

#include <array>
#include <cstddef>

namespace Horo::Editor::Detail {
    /** @brief Returns row-major scalar components for one Mat4 or the fixed four-cascade Mat4 array.
     * @param columnMajor Original scene matrices, contiguous in array-major then column-major order.
     * @return Fixed stack-owned values obeying MaterialParameterValue; native packing stays with the shared writer.
     */
    template <std::size_t Count>
    [[nodiscard]] std::array<float, Count> MatrixRowMajorComponents(const std::array<float, Count> &columnMajor) noexcept {
        static_assert(Count == 16 || Count == 64);
        std::array<float, Count> components;
        for (std::size_t at = 0; at < Count; ++at) {
            const auto matrix = at / 16;
            const auto row = (at % 16) / 4;
            const auto column = at % 4;
            components[at] = columnMajor[matrix * 16 + column * 4 + row];
        }
        return components;
    }
}  // namespace Horo::Editor::Detail
