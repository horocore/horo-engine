#pragma once

#include "Horo/Runtime/Render/RenderGraph.h"

#include <cstdint>

namespace Horo::Render::Detail {
    [[nodiscard]] constexpr bool IsKnown(const RenderGraphAccess access) noexcept {
        return static_cast<std::uint8_t>(access) <= static_cast<std::uint8_t>(RenderGraphAccess::ReadWrite);
    }

    [[nodiscard]] constexpr bool IsKnown(const RenderGraphUsageKind kind) noexcept {
        return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(RenderGraphUsageKind::UniformBuffer);
    }

    [[nodiscard]] constexpr bool IsKnown(const RenderGraphDependencyKind kind) noexcept {
        return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(RenderGraphDependencyKind::ExternalSynchronization);
    }

    [[nodiscard]] constexpr bool IsKnown(const RenderGraphPassCullPolicy policy) noexcept {
        return static_cast<std::uint8_t>(policy) <= static_cast<std::uint8_t>(RenderGraphPassCullPolicy::AllowCullIfOutputsUnused);
    }
}  // namespace Horo::Render::Detail
