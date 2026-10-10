#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <array>

namespace Horo::Render::Detail {
    class RenderResourceRegistry;
    struct RenderGraphTransientResourceSet;

    /** @brief Bounded owner-thread resolution; it neither acquires pins nor publishes backend state. */
    struct ResolvedFrameGraphResources final {
        std::array<RenderGraphResourceInstance, RenderGraphLimits::HardMaxResources> instances;
        std::size_t count{};

        [[nodiscard]] std::span<const RenderGraphResourceInstance> View() const noexcept {
            return std::span{instances}.first(count);
        }
    };

    /** @brief Resolves exact resident generations synchronously without escaping the owner registry. */
    [[nodiscard]] Result<ResolvedFrameGraphResources> ResolveFrameGraphResources(
        const CompiledRenderGraphExecution &graph, const RenderResourceRegistry &registry,
        const RenderGraphTransientResourceSet *transient = nullptr);
}  // namespace Horo::Render::Detail
