#pragma once
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <array>

namespace Horo::Render::Detail {
    class RenderResourceRegistry;

    /** @brief Stack-bounded exact generation mappings; spans are borrowed only during synchronous backend admission. */
    struct ResolvedMultiviewResources final {
        std::array<RenderGraphMeshInstance, MultiviewExtractionLimits::HardMaximumInstances> meshes;
        std::array<RenderGraphTextureViewInstance, MultiviewExtractionLimits::HardMaximumViews * 2> views;
        std::size_t meshCount{}, viewCount{};
    };

    /** @brief Resolves real resident meshes and attachment views before the canonical graph lease is acquired. */
    [[nodiscard]] Result<ResolvedMultiviewResources> ResolveMultiviewResources(std::span<const RenderGraphPassWorkload> workloads,
                                                                               const RenderResourceRegistry &registry);
}  // namespace Horo::Render::Detail
