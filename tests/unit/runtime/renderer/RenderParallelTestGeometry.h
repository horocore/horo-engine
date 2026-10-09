#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#include <vector>

namespace Horo::Render::Test {
    /** @brief Owned triangle producer fixture shared by capture rebinding and source-destruction regressions. */
    inline std::vector<MeshVertex> MakeParallelTriangle() {
        return {{{0, 0, 0}, {0, 0, 1}, {0, 0}}, {{1, 0, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0, 1}}};
    }
}  // namespace Horo::Render::Test
