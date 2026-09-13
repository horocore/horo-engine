#pragma once

#include "Horo/Runtime/Render/ShaderManifest.h"

#include <vector>

namespace Horo::Tests {
    /** @brief Standard three-resource surface binding fixture shared by shader contract suites. */
    [[nodiscard]] inline std::vector<Render::ShaderResourceBinding> StandardSurfaceBindings() {
        using namespace Render;
        return {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                 ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                 ShaderStageVisibility::Fragment},
                {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
    }
}  // namespace Horo::Tests
