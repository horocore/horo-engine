#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <unordered_map>

namespace Horo::Render::Detail {
    /** @brief Headless native-instance metadata, bounded by the resident registry's capacity. */
    struct NullGraphResources {
        static constexpr std::size_t MaximumResources = 65'535;
        std::unordered_map<std::uint64_t, RenderBufferDescriptor> buffers;
        std::unordered_map<std::uint64_t, RenderTextureDescriptor> textures;
    };

    /**
     * @brief Validates actual headless instances and workloads after portable graph admission.
     * @param resources Backend-owned ready instance metadata.
     * @param request Exact synchronous graph request.
     * @return Success or a typed identity, operation, range, or unsupported result.
     */
    [[nodiscard]] Result<void> ValidateNullGraphWorkloads(const NullGraphResources &resources, const RenderGraphExecutionRequest &request);
}  // namespace Horo::Render::Detail
